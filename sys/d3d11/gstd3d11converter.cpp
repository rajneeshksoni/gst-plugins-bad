/* GStreamer
 * Copyright (C) <2019> Seungha Yang <seungha.yang@navercorp.com>
 * Copyright (C) <2019> Jeongki Kim <jeongki.kim@jeongki.kim>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "gstd3d11converter.h"
#include "gstd3d11shader.h"
#include "gstd3d11pluginutils.h"
#include <wrl.h>
#include <string.h>

/* *INDENT-OFF* */
using namespace Microsoft::WRL;

G_BEGIN_DECLS

GST_DEBUG_CATEGORY_EXTERN (gst_d3d11_converter_debug);
#define GST_CAT_DEFAULT gst_d3d11_converter_debug

G_END_DECLS
/* *INDENT-ON* */

#define CONVERTER_MAX_QUADS 2

/* *INDENT-OFF* */
typedef struct
{
  FLOAT trans_matrix[12];
  FLOAT padding[4];
} PixelShaderColorTransform;

typedef struct
{
  struct {
    FLOAT x;
    FLOAT y;
    FLOAT z;
  } position;
  struct {
    FLOAT x;
    FLOAT y;
  } texture;
} VertexData;

typedef struct
{
  const gchar *constant_buffer;
  const gchar *func;
} PixelShaderTemplate;

#define COLOR_TRANSFORM_COEFF \
    "cbuffer PixelShaderColorTransform : register(b0)\n" \
    "{\n" \
    "  float3x4 trans_matrix;\n" \
    "  float3 padding;\n" \
    "};\n"

#define HLSL_FUNC_YUV_TO_RGB \
    "float3 yuv_to_rgb (float3 yuv)\n" \
    "{\n" \
    "  yuv += float3(-0.062745f, -0.501960f, -0.501960f);\n" \
    "  yuv = mul(yuv, trans_matrix);\n" \
    "  return saturate(yuv);\n" \
    "}\n"

#define HLSL_FUNC_RGB_TO_YUV \
    "float3 rgb_to_yuv (float3 rgb)\n" \
    "{\n" \
    "  float3 yuv;\n" \
    "  yuv = mul(rgb, trans_matrix);\n" \
    "  yuv += float3(0.062745f, 0.501960f, 0.501960f);\n" \
    "  return saturate(yuv);\n" \
    "}\n"

#define HLSL_PS_OUTPUT_ONE_PLANE_BODY \
    "  float4 Plane_0: SV_TARGET0;"

#define HLSL_PS_OUTPUT_TWO_PLANES_BODY \
    "  float4 Plane_0: SV_TARGET0;\n" \
    "  float4 Plane_1: SV_TARGET1;"

static const PixelShaderTemplate templ_REORDER =
    { NULL, NULL };

static const PixelShaderTemplate templ_YUV_to_RGB =
    { COLOR_TRANSFORM_COEFF, HLSL_FUNC_YUV_TO_RGB };

static const PixelShaderTemplate templ_RGB_to_YUV =
    { COLOR_TRANSFORM_COEFF, HLSL_FUNC_RGB_TO_YUV };

static const gchar templ_REORDER_BODY[] =
    "  float4 xyza;\n"
    "  xyza.xyz = shaderTexture[0].Sample(samplerState, input.Texture).xyz;\n"
    "  xyza.a = shaderTexture[0].Sample(samplerState, input.Texture).a * %f;\n"
    "  output.Plane_0 = xyza;\n";

static const gchar templ_VUYA_to_RGB_BODY[] =
    "  float4 sample, rgba;\n"
    "  sample.x  = shaderTexture[0].Sample(samplerState, input.Texture).z;\n"
    "  sample.y  = shaderTexture[0].Sample(samplerState, input.Texture).y;\n"
    "  sample.z  = shaderTexture[0].Sample(samplerState, input.Texture).x;\n"
    "  sample.a  = shaderTexture[0].Sample(samplerState, input.Texture).a;\n"
    "  rgba.rgb = yuv_to_rgb (sample.xyz);\n"
    "  rgba.a = sample.a;\n"
    "  output.Plane_0 = rgba;\n";

static const gchar templ_RGB_to_VUYA_BODY[] =
    "  float4 sample, vuya;\n"
    "  sample = shaderTexture[0].Sample(samplerState, input.Texture);\n"
    "  vuya.zyx = rgb_to_yuv (sample.rgb);\n"
    "  vuya.a = sample.a;\n"
    "  output.Plane_0 = vuya;\n";

static const gchar templ_PACKED_YUV_to_RGB_BODY[] =
    "  float4 sample, rgba;\n"
    "  sample.x  = shaderTexture[0].Sample(samplerState, input.Texture).%c;\n"
    "  sample.y  = shaderTexture[0].Sample(samplerState, input.Texture).%c;\n"
    "  sample.z  = shaderTexture[0].Sample(samplerState, input.Texture).%c;\n"
    "  rgba.rgb = yuv_to_rgb (sample.xyz);\n"
    "  rgba.a = 1;\n"
    "  output.Plane_0 = rgba;\n";

/* YUV to RGB conversion */
static const gchar templ_PLANAR_YUV_to_RGB_BODY[] =
    "  float4 sample, rgba;\n"
    "  sample.x  = shaderTexture[0].Sample(samplerState, input.Texture).x * %d;\n"
    "  sample.y  = shaderTexture[1].Sample(samplerState, input.Texture).x * %d;\n"
    "  sample.z  = shaderTexture[2].Sample(samplerState, input.Texture).x * %d;\n"
    "  rgba.rgb = yuv_to_rgb (sample.xyz);\n"
    "  rgba.a = 1.0;\n"
    "  output.Plane_0 = rgba;\n";

static const gchar templ_SEMI_PLANAR_to_RGB_BODY[] =
    "  float4 sample, rgba;\n"
    "  sample.x  = shaderTexture[0].Sample(samplerState, input.Texture).x;\n"
    "  sample.yz = shaderTexture[1].Sample(samplerState, input.Texture).xy;\n"
    "  rgba.rgb = yuv_to_rgb (sample.xyz);\n"
    "  rgba.a = 1.0;\n"
    "  output.Plane_0 = rgba;\n";

/* RGB to YUV conversion */
static const gchar templ_RGB_to_LUMA_BODY[] =
    "  float4 sample, rgba;\n"
    "  rgba.rgb = shaderTexture[0].Sample(samplerState, input.Texture).rgb;\n"
    "  sample.xyz = rgb_to_yuv (rgba.rgb);\n"
    "  sample.y = 0.0;\n"
    "  sample.z = 0.0;\n"
    "  sample.a = 0.0;\n"
    "  sample.x = sample.x / %d;\n"
    "  output.Plane_0 = sample;\n";

static const gchar templ_RGB_to_SEMI_PLANAR_CHROMA_BODY[] =
    "  float4 sample, rgba;\n"
    "  rgba.rgb = shaderTexture[0].Sample(samplerState, input.Texture).rgb;\n"
    "  sample.xyz = rgb_to_yuv (rgba.rgb);\n"
    "  sample.x = sample.y;\n"
    "  sample.y = sample.z;\n"
    "  sample.z = 0.0;\n"
    "  sample.a = 0.0;\n"
    "  output.Plane_0 = sample;\n";

static const gchar templ_RGB_to_PLANAR_CHROMA_BODY[] =
    "  float4 sample, rgba;\n"
    "  rgba.rgb = shaderTexture[0].Sample(samplerState, input.Texture).rgb;\n"
    "  sample.xyz = rgb_to_yuv (rgba.rgb);\n"
    "  output.Plane_0 = float4(sample.y / %d, 0.0, 0.0, 0.0);\n"
    "  output.Plane_1 = float4(sample.z / %d, 0.0, 0.0, 0.0);\n";

/* YUV to YUV conversion */
static const gchar templ_LUMA_to_LUMA_BODY[] =
    "  float4 sample;\n"
    "  sample.x = shaderTexture[0].Sample(samplerState, input.Texture).x * %d;\n"
    "  output.Plane_0 = float4(sample.x / %d, 0.0, 0.0, 0.0);\n";

static const gchar templ_PLANAR_TO_SEMI_PLANAR_CHROMA_BODY[] =
    "  float4 sample;\n"
    "  sample.y = shaderTexture[1].Sample(samplerState, input.Texture).x * %d;\n"
    "  sample.z = shaderTexture[2].Sample(samplerState, input.Texture).x * %d;\n"
    "  output.Plane_0 = float4(sample.yz, 0.0, 0.0);\n";

static const gchar templ_SEMI_PLANAR_TO_PLANAR_CHROMA_BODY[] =
    "  float4 sample;\n"
    "  sample.yz = shaderTexture[1].Sample(samplerState, input.Texture).xy;\n"
    "  output.Plane_0 = float4(sample.y / %d, 0.0, 0.0, 0.0);\n"
    "  output.Plane_1 = float4(sample.z / %d, 0.0, 0.0, 0.0);\n";

static const gchar templ_SEMI_PLANAR_TO_SEMI_PLANAR_CHROMA_BODY[] =
    "  float4 sample;\n"
    "  sample.yz = shaderTexture[1].Sample(samplerState, input.Texture).xy;\n"
    "  output.Plane_0 = float4(sample.yz, 0.0, 0.0);\n";

static const gchar templ_PLANAR_TO_PLANAR_CHROMA_BODY[] =
    "  float4 sample;\n"
    "  sample.y = shaderTexture[1].Sample(samplerState, input.Texture).x * %d;\n"
    "  sample.z = shaderTexture[2].Sample(samplerState, input.Texture).x * %d;\n"
    "  output.Plane_0 = float4(sample.y / %d, 0.0, 0.0, 0.0);\n"
    "  output.Plane_1 = float4(sample.z / %d, 0.0, 0.0, 0.0);\n";

/* VUYA to YUV */
static const gchar templ_VUYA_to_LUMA_BODY[] =
    "  float4 sample;\n"
    "  sample.x = shaderTexture[0].Sample(samplerState, input.Texture).z;\n"
    "  output.Plane_0 = float4(sample.x / %d, 0.0, 0.0, 0.0);\n";

static const gchar templ_VUYA_TO_PLANAR_CHROMA_BODY[] =
    "  float4 sample;\n"
    "  sample.yz = shaderTexture[0].Sample(samplerState, input.Texture).yx;\n"
    "  output.Plane_0 = float4(sample.y / %d, 0.0, 0.0, 0.0);\n"
    "  output.Plane_1 = float4(sample.z / %d, 0.0, 0.0, 0.0);\n";

static const gchar templ_VUYA_TO_SEMI_PLANAR_CHROMA_BODY[] =
    "  float4 sample;\n"
    "  sample.yz = shaderTexture[0].Sample(samplerState, input.Texture).yx;\n"
    "  output.Plane_0 = float4(sample.yz, 0.0, 0.0);\n";

/* YUV to VUYA */
static const gchar templ_PLANAR_to_VUYA_BODY[] =
    "  float4 sample;\n"
    "  sample.z = shaderTexture[0].Sample(samplerState, input.Texture).x * %d;\n"
    "  sample.y = shaderTexture[1].Sample(samplerState, input.Texture).x * %d;\n"
    "  sample.x = shaderTexture[2].Sample(samplerState, input.Texture).x * %d;\n"
    "  output.Plane_0 = float4(sample.xyz, 1.0f);\n";

static const gchar templ_SEMI_PLANAR_to_VUYA_BODY[] =
    "  float4 sample;\n"
    "  sample.z = shaderTexture[0].Sample(samplerState, input.Texture).x;\n"
    "  sample.xy = shaderTexture[1].Sample(samplerState, input.Texture).yx;\n"
    "  output.Plane_0 = float4(sample.xyz, 1.0f);\n";

static const gchar templ_PACKED_YUV_to_VUYA_BODY[] =
    "  float4 sample;\n"
    "  sample.z = shaderTexture[0].Sample(samplerState, input.Texture).%c;\n"
    "  sample.y = shaderTexture[0].Sample(samplerState, input.Texture).%c;\n"
    "  sample.x = shaderTexture[0].Sample(samplerState, input.Texture).%c;\n"
    "  output.Plane_0 = float4(sample.xyz, 1.0f);\n";

/* packed YUV to (semi) planar YUV */
static const gchar templ_PACKED_YUV_to_LUMA_BODY[] =
    "  float4 sample;\n"
    "  sample.x = shaderTexture[0].Sample(samplerState, input.Texture).%c;\n"
    "  output.Plane_0 = float4(sample.x / %d, 0.0, 0.0, 0.0);\n";

static const gchar templ_PACKED_YUV_TO_PLANAR_CHROMA_BODY[] =
    "  float4 sample;\n"
    "  sample.y = shaderTexture[0].Sample(samplerState, input.Texture).%c;\n"
    "  sample.z = shaderTexture[0].Sample(samplerState, input.Texture).%c;\n"
    "  output.Plane_0 = float4(sample.y / %d, 0.0, 0.0, 0.0);\n"
    "  output.Plane_1 = float4(sample.z / %d, 0.0, 0.0, 0.0);\n";

static const gchar templ_PACKED_YUV_TO_SEMI_PLANAR_CHROMA_BODY[] =
    "  float4 sample;\n"
    "  sample.y = shaderTexture[0].Sample(samplerState, input.Texture).%c;\n"
    "  sample.z = shaderTexture[0].Sample(samplerState, input.Texture).%c;\n"
    "  output.Plane_0 = float4(sample.yz, 0.0, 0.0);\n";

static const gchar templ_pixel_shader[] =
    /* constant buffer */
    "%s\n"
    "Texture2D shaderTexture[4];\n"
    "SamplerState samplerState;\n"
    "\n"
    "struct PS_INPUT\n"
    "{\n"
    "  float4 Position: SV_POSITION;\n"
    "  float3 Texture: TEXCOORD0;\n"
    "};\n"
    "\n"
    "struct PS_OUTPUT\n"
    "{\n"
    "  %s\n"
    "};\n"
    "\n"
    /* rgb <-> yuv function */
    "%s\n"
    "PS_OUTPUT main(PS_INPUT input)\n"
    "{\n"
    "  PS_OUTPUT output;\n"
    "%s"
    "  return output;\n"
    "}\n";

static const gchar templ_vertex_shader[] =
    "struct VS_INPUT\n"
    "{\n"
    "  float4 Position : POSITION;\n"
    "  float4 Texture : TEXCOORD0;\n"
    "};\n"
    "\n"
    "struct VS_OUTPUT\n"
    "{\n"
    "  float4 Position: SV_POSITION;\n"
    "  float4 Texture: TEXCOORD0;\n"
    "};\n"
    "\n"
    "VS_OUTPUT main(VS_INPUT input)\n"
    "{\n"
    "  return input;\n"
    "}\n";

/* *INDENT-ON* */

typedef struct
{
  const PixelShaderTemplate *templ;
  gchar *ps_body[CONVERTER_MAX_QUADS];
  const gchar *ps_output[CONVERTER_MAX_QUADS];
  PixelShaderColorTransform transform;
} ConvertInfo;

struct _GstD3D11Converter
{
  GstD3D11Device *device;
  GstVideoInfo in_info;
  GstVideoInfo out_info;
  gfloat alpha;

  const GstD3D11Format *in_d3d11_format;
  const GstD3D11Format *out_d3d11_format;

  guint num_input_view;
  guint num_output_view;

  GstD3D11Quad *quad[CONVERTER_MAX_QUADS];

  D3D11_VIEWPORT viewport[GST_VIDEO_MAX_PLANES];

  RECT src_rect;
  RECT dest_rect;
  gint input_texture_width;
  gint input_texture_height;
  ID3D11Buffer *vertex_buffer;
  gboolean update_vertex;

  ConvertInfo convert_info;
};

/* from video-converter.c */
typedef struct
{
  gfloat dm[4][4];
} MatrixData;

static void
color_matrix_set_identity (MatrixData * m)
{
  gint i, j;

  for (i = 0; i < 4; i++) {
    for (j = 0; j < 4; j++) {
      m->dm[i][j] = (i == j);
    }
  }
}

static void
color_matrix_copy (MatrixData * d, const MatrixData * s)
{
  gint i, j;

  for (i = 0; i < 4; i++)
    for (j = 0; j < 4; j++)
      d->dm[i][j] = s->dm[i][j];
}

/* Perform 4x4 matrix multiplication:
 *  - @dst@ = @a@ * @b@
 *  - @dst@ may be a pointer to @a@ andor @b@
 */
static void
color_matrix_multiply (MatrixData * dst, MatrixData * a, MatrixData * b)
{
  MatrixData tmp;
  gint i, j, k;

  for (i = 0; i < 4; i++) {
    for (j = 0; j < 4; j++) {
      gfloat x = 0;
      for (k = 0; k < 4; k++) {
        x += a->dm[i][k] * b->dm[k][j];
      }
      tmp.dm[i][j] = x;
    }
  }
  color_matrix_copy (dst, &tmp);
}

static void
color_matrix_offset_components (MatrixData * m, gfloat a1, gfloat a2, gfloat a3)
{
  MatrixData a;

  color_matrix_set_identity (&a);
  a.dm[0][3] = a1;
  a.dm[1][3] = a2;
  a.dm[2][3] = a3;
  color_matrix_multiply (m, &a, m);
}

static void
color_matrix_scale_components (MatrixData * m, gfloat a1, gfloat a2, gfloat a3)
{
  MatrixData a;

  color_matrix_set_identity (&a);
  a.dm[0][0] = a1;
  a.dm[1][1] = a2;
  a.dm[2][2] = a3;
  color_matrix_multiply (m, &a, m);
}

static void
color_matrix_debug (GstD3D11Converter * self, const MatrixData * s)
{
  GST_DEBUG ("[%f %f %f %f]",
      s->dm[0][0], s->dm[0][1], s->dm[0][2], s->dm[0][3]);
  GST_DEBUG ("[%f %f %f %f]",
      s->dm[1][0], s->dm[1][1], s->dm[1][2], s->dm[1][3]);
  GST_DEBUG ("[%f %f %f %f]",
      s->dm[2][0], s->dm[2][1], s->dm[2][2], s->dm[2][3]);
  GST_DEBUG ("[%f %f %f %f]",
      s->dm[3][0], s->dm[3][1], s->dm[3][2], s->dm[3][3]);
}

static void
color_matrix_YCbCr_to_RGB (MatrixData * m, gfloat Kr, gfloat Kb)
{
  gfloat Kg = 1.0 - Kr - Kb;
  MatrixData k = {
    {
          {1., 0., 2 * (1 - Kr), 0.},
          {1., -2 * Kb * (1 - Kb) / Kg, -2 * Kr * (1 - Kr) / Kg, 0.},
          {1., 2 * (1 - Kb), 0., 0.},
          {0., 0., 0., 1.},
        }
  };

  color_matrix_multiply (m, &k, m);
}

static void
color_matrix_RGB_to_YCbCr (MatrixData * m, gfloat Kr, gfloat Kb)
{
  gfloat Kg = 1.0 - Kr - Kb;
  MatrixData k;
  gfloat x;

  k.dm[0][0] = Kr;
  k.dm[0][1] = Kg;
  k.dm[0][2] = Kb;
  k.dm[0][3] = 0;

  x = 1 / (2 * (1 - Kb));
  k.dm[1][0] = -x * Kr;
  k.dm[1][1] = -x * Kg;
  k.dm[1][2] = x * (1 - Kb);
  k.dm[1][3] = 0;

  x = 1 / (2 * (1 - Kr));
  k.dm[2][0] = x * (1 - Kr);
  k.dm[2][1] = -x * Kg;
  k.dm[2][2] = -x * Kb;
  k.dm[2][3] = 0;

  k.dm[3][0] = 0;
  k.dm[3][1] = 0;
  k.dm[3][2] = 0;
  k.dm[3][3] = 1;

  color_matrix_multiply (m, &k, m);
}

static void
compute_matrix_to_RGB (GstD3D11Converter * self, MatrixData * data,
    GstVideoInfo * info)
{
  gdouble Kr = 0, Kb = 0;
  gint offset[4], scale[4];

  /* bring color components to [0..1.0] range */
  gst_video_color_range_offsets (info->colorimetry.range, info->finfo, offset,
      scale);

  color_matrix_offset_components (data, -offset[0], -offset[1], -offset[2]);
  color_matrix_scale_components (data, 1 / ((float) scale[0]),
      1 / ((float) scale[1]), 1 / ((float) scale[2]));

  if (!GST_VIDEO_INFO_IS_RGB (info)) {
    /* bring components to R'G'B' space */
    if (gst_video_color_matrix_get_Kr_Kb (info->colorimetry.matrix, &Kr, &Kb))
      color_matrix_YCbCr_to_RGB (data, Kr, Kb);
  }
  color_matrix_debug (self, data);
}

static void
compute_matrix_to_YUV (GstD3D11Converter * self, MatrixData * data,
    GstVideoInfo * info)
{
  gdouble Kr = 0, Kb = 0;
  gint offset[4], scale[4];

  if (!GST_VIDEO_INFO_IS_RGB (info)) {
    /* bring components to YCbCr space */
    if (gst_video_color_matrix_get_Kr_Kb (info->colorimetry.matrix, &Kr, &Kb))
      color_matrix_RGB_to_YCbCr (data, Kr, Kb);
  }

  /* bring color components to nominal range */
  gst_video_color_range_offsets (info->colorimetry.range, info->finfo, offset,
      scale);

  color_matrix_scale_components (data, (float) scale[0], (float) scale[1],
      (float) scale[2]);
  color_matrix_offset_components (data, offset[0], offset[1], offset[2]);

  color_matrix_debug (self, data);
}

static gboolean
converter_get_matrix (GstD3D11Converter * self, MatrixData * matrix,
    GstVideoInfo * in_info, GstVideoInfo * out_info)
{
  gboolean same_matrix;
  guint in_bits, out_bits;

  in_bits = GST_VIDEO_INFO_COMP_DEPTH (in_info, 0);
  out_bits = GST_VIDEO_INFO_COMP_DEPTH (out_info, 0);

  same_matrix = in_info->colorimetry.matrix == out_info->colorimetry.matrix;

  GST_DEBUG ("matrix %d -> %d (%d)", in_info->colorimetry.matrix,
      out_info->colorimetry.matrix, same_matrix);

  color_matrix_set_identity (matrix);

  if (same_matrix) {
    GST_DEBUG ("conversion matrix is not required");
    return FALSE;
  }

  if (in_bits < out_bits) {
    gint scale = 1 << (out_bits - in_bits);
    color_matrix_scale_components (matrix,
        1 / (float) scale, 1 / (float) scale, 1 / (float) scale);
  }

  GST_DEBUG ("to RGB matrix");
  compute_matrix_to_RGB (self, matrix, in_info);
  GST_DEBUG ("current matrix");
  color_matrix_debug (self, matrix);

  GST_DEBUG ("to YUV matrix");
  compute_matrix_to_YUV (self, matrix, out_info);
  GST_DEBUG ("current matrix");
  color_matrix_debug (self, matrix);

  if (in_bits > out_bits) {
    gint scale = 1 << (in_bits - out_bits);
    color_matrix_scale_components (matrix,
        (float) scale, (float) scale, (float) scale);
  }

  GST_DEBUG ("final matrix");
  color_matrix_debug (self, matrix);

  return TRUE;
}

static gboolean
setup_convert_info_rgb_to_rgb (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *convert_info = &self->convert_info;

  convert_info->templ = &templ_REORDER;
  convert_info->ps_body[0] = g_strdup_printf (templ_REORDER_BODY, self->alpha);
  convert_info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;

  return TRUE;
}

static gboolean
get_packed_yuv_components (GstD3D11Converter * self, GstVideoFormat
    format, gchar * y, gchar * u, gchar * v)
{
  switch (format) {
    case GST_VIDEO_FORMAT_YUY2:
    {
      const GstD3D11Format *d3d11_format =
          gst_d3d11_device_format_from_gst (self->device,
          GST_VIDEO_FORMAT_YUY2);

      g_assert (d3d11_format != NULL);

      if (d3d11_format->resource_format[0] == DXGI_FORMAT_R8G8B8A8_UNORM) {
        *y = 'x';
        *u = 'y';
        *v = 'a';
      } else if (d3d11_format->resource_format[0] ==
          DXGI_FORMAT_G8R8_G8B8_UNORM) {
        *y = 'y';
        *u = 'x';
        *v = 'z';
      } else {
        g_assert_not_reached ();
        return FALSE;
      }
      break;
    }
    case GST_VIDEO_FORMAT_UYVY:
      *y = 'y';
      *u = 'x';
      *v = 'z';
      break;
    case GST_VIDEO_FORMAT_VYUY:
      *y = 'y';
      *u = 'z';
      *v = 'x';
      break;
    case GST_VIDEO_FORMAT_Y210:
      *y = 'r';
      *u = 'g';
      *v = 'a';
      break;
    case GST_VIDEO_FORMAT_Y410:
      *y = 'g';
      *u = 'r';
      *v = 'b';
      break;
    default:
      g_assert_not_reached ();
      return FALSE;
  }

  return TRUE;
}

static gboolean
setup_convert_info_yuv_to_rgb (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *info = &self->convert_info;

  info->templ = &templ_YUV_to_RGB;
  info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;

  switch (GST_VIDEO_INFO_FORMAT (in_info)) {
    case GST_VIDEO_FORMAT_VUYA:
      info->ps_body[0] = g_strdup_printf (templ_VUYA_to_RGB_BODY);
      break;
    case GST_VIDEO_FORMAT_YUY2:
    case GST_VIDEO_FORMAT_UYVY:
    case GST_VIDEO_FORMAT_VYUY:
    case GST_VIDEO_FORMAT_Y210:
    case GST_VIDEO_FORMAT_Y410:
    {
      gchar y, u, v;
      if (!get_packed_yuv_components (self, GST_VIDEO_INFO_FORMAT (in_info),
              &y, &u, &v)) {
        return FALSE;
      }

      info->ps_body[0] =
          g_strdup_printf (templ_PACKED_YUV_to_RGB_BODY, y, u, v);
      break;
    }
    case GST_VIDEO_FORMAT_I420:
      info->ps_body[0] =
          g_strdup_printf (templ_PLANAR_YUV_to_RGB_BODY, 1, 1, 1);
      break;
    case GST_VIDEO_FORMAT_I420_10LE:
      info->ps_body[0] =
          g_strdup_printf (templ_PLANAR_YUV_to_RGB_BODY, 64, 64, 64);
      break;
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_P010_10LE:
    case GST_VIDEO_FORMAT_P016_LE:
      info->ps_body[0] = g_strdup_printf (templ_SEMI_PLANAR_to_RGB_BODY);
      break;
    default:
      GST_FIXME_OBJECT (self,
          "Unhandled input format %s",
          gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (in_info)));
      return FALSE;
  }

  return TRUE;
}

static gboolean
setup_convert_info_rgb_to_yuv (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *info = &self->convert_info;

  info->templ = &templ_RGB_to_YUV;
  info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;

  switch (GST_VIDEO_INFO_FORMAT (out_info)) {
    case GST_VIDEO_FORMAT_VUYA:
      info->ps_body[0] = g_strdup_printf (templ_RGB_to_VUYA_BODY);
      break;
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_P010_10LE:
    case GST_VIDEO_FORMAT_P016_LE:
      info->ps_body[0] = g_strdup_printf (templ_RGB_to_LUMA_BODY, 1);
      info->ps_body[1] = g_strdup_printf (templ_RGB_to_SEMI_PLANAR_CHROMA_BODY);
      info->ps_output[1] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;
      break;
    case GST_VIDEO_FORMAT_I420:
      info->ps_body[0] = g_strdup_printf (templ_RGB_to_LUMA_BODY, 1);
      info->ps_body[1] =
          g_strdup_printf (templ_RGB_to_PLANAR_CHROMA_BODY, 1, 1);
      info->ps_output[1] = HLSL_PS_OUTPUT_TWO_PLANES_BODY;
      break;
    case GST_VIDEO_FORMAT_I420_10LE:
      info->ps_body[0] = g_strdup_printf (templ_RGB_to_LUMA_BODY, 64);
      info->ps_body[1] =
          g_strdup_printf (templ_RGB_to_PLANAR_CHROMA_BODY, 64, 64);
      info->ps_output[1] = HLSL_PS_OUTPUT_TWO_PLANES_BODY;
      break;
    default:
      GST_FIXME_OBJECT (self,
          "Unhandled output format %s",
          gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (out_info)));
      return FALSE;
  }

  return TRUE;
}

static gboolean
setup_convert_info_planar_to_planar (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *info = &self->convert_info;
  gint mul = 1;
  gint div = 1;

  info->templ = &templ_REORDER;
  info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;
  info->ps_output[1] = HLSL_PS_OUTPUT_TWO_PLANES_BODY;

  if (GST_VIDEO_INFO_FORMAT (in_info) == GST_VIDEO_FORMAT_I420_10LE)
    mul = 64;

  if (GST_VIDEO_INFO_FORMAT (out_info) == GST_VIDEO_FORMAT_I420_10LE)
    div = 64;

  info->ps_body[0] = g_strdup_printf (templ_LUMA_to_LUMA_BODY, mul, div);
  info->ps_body[1] =
      g_strdup_printf (templ_PLANAR_TO_PLANAR_CHROMA_BODY, mul, mul, div, div);

  return TRUE;
}

static gboolean
setup_convert_info_planar_to_semi_planar (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *info = &self->convert_info;
  gint mul = 1;
  gint div = 1;

  info->templ = &templ_REORDER;
  info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;
  info->ps_output[1] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;

  if (GST_VIDEO_INFO_FORMAT (in_info) == GST_VIDEO_FORMAT_I420_10LE)
    mul = 64;

  info->ps_body[0] = g_strdup_printf (templ_LUMA_to_LUMA_BODY, mul, div);
  info->ps_body[1] =
      g_strdup_printf (templ_PLANAR_TO_SEMI_PLANAR_CHROMA_BODY, mul, mul);

  return TRUE;
}

static gboolean
setup_convert_info_semi_planar_to_planar (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *info = &self->convert_info;
  gint mul = 1;
  gint div = 1;

  info->templ = &templ_REORDER;
  info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;
  info->ps_output[1] = HLSL_PS_OUTPUT_TWO_PLANES_BODY;

  if (GST_VIDEO_INFO_FORMAT (out_info) == GST_VIDEO_FORMAT_I420_10LE)
    div = 64;

  info->ps_body[0] = g_strdup_printf (templ_LUMA_to_LUMA_BODY, mul, div);
  info->ps_body[1] =
      g_strdup_printf (templ_SEMI_PLANAR_TO_PLANAR_CHROMA_BODY, div, div);

  return TRUE;
}

static gboolean
setup_convert_info_semi_planar_to_semi_planar (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *info = &self->convert_info;
  gint mul = 1;
  gint div = 1;

  info->templ = &templ_REORDER;
  info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;
  info->ps_output[1] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;

  info->ps_body[0] = g_strdup_printf (templ_LUMA_to_LUMA_BODY, mul, div);
  info->ps_body[1] =
      g_strdup_printf (templ_SEMI_PLANAR_TO_SEMI_PLANAR_CHROMA_BODY);

  return TRUE;
}

static gboolean
setup_convert_info_vuya_to_vuya (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *info = &self->convert_info;

  info->templ = &templ_REORDER;
  info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;

  info->ps_body[0] = g_strdup_printf (templ_REORDER_BODY, self->alpha);

  return TRUE;
}

static gboolean
setup_convert_info_vuya_to_planar (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *info = &self->convert_info;
  gint div = 1;

  info->templ = &templ_REORDER;
  info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;
  info->ps_output[1] = HLSL_PS_OUTPUT_TWO_PLANES_BODY;

  if (GST_VIDEO_INFO_FORMAT (out_info) == GST_VIDEO_FORMAT_I420_10LE)
    div = 64;

  info->ps_body[0] = g_strdup_printf (templ_VUYA_to_LUMA_BODY, div);
  info->ps_body[1] =
      g_strdup_printf (templ_VUYA_TO_PLANAR_CHROMA_BODY, div, div);

  return TRUE;
}

static gboolean
setup_convert_info_vuya_to_semi_planar (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *info = &self->convert_info;
  gint div = 1;

  info->templ = &templ_REORDER;
  info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;
  info->ps_output[1] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;

  info->ps_body[0] = g_strdup_printf (templ_VUYA_to_LUMA_BODY, div);
  info->ps_body[1] = g_strdup_printf (templ_VUYA_TO_SEMI_PLANAR_CHROMA_BODY);

  return TRUE;
}

static gboolean
setup_convert_info_planar_to_vuya (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *info = &self->convert_info;
  gint mul = 1;

  info->templ = &templ_REORDER;
  info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;

  if (GST_VIDEO_INFO_FORMAT (in_info) == GST_VIDEO_FORMAT_I420_10LE)
    mul = 64;

  info->ps_body[0] = g_strdup_printf (templ_PLANAR_to_VUYA_BODY, mul, mul, mul);

  return TRUE;
}

static gboolean
setup_convert_info_packed_yuv_to_vuya (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *info = &self->convert_info;
  gchar y, u, v;

  info->templ = &templ_REORDER;
  info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;

  if (!get_packed_yuv_components (self, GST_VIDEO_INFO_FORMAT (in_info),
          &y, &u, &v)) {
    return FALSE;
  }

  info->ps_body[0] = g_strdup_printf (templ_PACKED_YUV_to_VUYA_BODY, y, u, v);

  return TRUE;
}

static gboolean
setup_convert_info_semi_planar_to_vuya (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *info = &self->convert_info;

  info->templ = &templ_REORDER;
  info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;

  info->ps_body[0] = g_strdup_printf (templ_SEMI_PLANAR_to_VUYA_BODY);

  return TRUE;
}

static gboolean
setup_convert_info_packed_yuv_to_planar (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *info = &self->convert_info;
  gint div = 1;
  gchar y, u, v;

  info->templ = &templ_REORDER;
  info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;
  info->ps_output[1] = HLSL_PS_OUTPUT_TWO_PLANES_BODY;

  if (GST_VIDEO_INFO_FORMAT (out_info) == GST_VIDEO_FORMAT_I420_10LE)
    div = 64;

  if (!get_packed_yuv_components (self, GST_VIDEO_INFO_FORMAT (in_info),
          &y, &u, &v)) {
    return FALSE;
  }

  info->ps_body[0] = g_strdup_printf (templ_PACKED_YUV_to_LUMA_BODY, y, div);
  info->ps_body[1] =
      g_strdup_printf (templ_PACKED_YUV_TO_PLANAR_CHROMA_BODY, u, v, div, div);

  return TRUE;
}

static gboolean
setup_convert_info_packed_yuv_to_semi_planar (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  ConvertInfo *info = &self->convert_info;
  gint div = 1;
  gchar y, u, v;

  info->templ = &templ_REORDER;
  info->ps_output[0] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;
  info->ps_output[1] = HLSL_PS_OUTPUT_ONE_PLANE_BODY;

  if (!get_packed_yuv_components (self, GST_VIDEO_INFO_FORMAT (in_info),
          &y, &u, &v)) {
    return FALSE;
  }

  info->ps_body[0] = g_strdup_printf (templ_PACKED_YUV_to_LUMA_BODY, y, div);
  info->ps_body[1] =
      g_strdup_printf (templ_PACKED_YUV_TO_SEMI_PLANAR_CHROMA_BODY, u, v);

  return TRUE;
}

static gboolean
setup_convert_info_yuv_to_yuv (GstD3D11Converter * self,
    const GstVideoInfo * in_info, const GstVideoInfo * out_info)
{
  gboolean in_planar, out_planar;
  gboolean in_vuya, out_vuya;
  gboolean in_packed;

  in_vuya = GST_VIDEO_INFO_FORMAT (in_info) == GST_VIDEO_FORMAT_VUYA;
  out_vuya = GST_VIDEO_INFO_FORMAT (out_info) == GST_VIDEO_FORMAT_VUYA;
  in_planar = (GST_VIDEO_INFO_FORMAT (in_info) == GST_VIDEO_FORMAT_I420 ||
      GST_VIDEO_INFO_FORMAT (in_info) == GST_VIDEO_FORMAT_I420_10LE);
  in_packed = (GST_VIDEO_INFO_FORMAT (in_info) == GST_VIDEO_FORMAT_YUY2 ||
      GST_VIDEO_INFO_FORMAT (in_info) == GST_VIDEO_FORMAT_UYVY ||
      GST_VIDEO_INFO_FORMAT (in_info) == GST_VIDEO_FORMAT_VYUY ||
      GST_VIDEO_INFO_FORMAT (in_info) == GST_VIDEO_FORMAT_Y210 ||
      GST_VIDEO_INFO_FORMAT (in_info) == GST_VIDEO_FORMAT_Y410);
  out_planar = (GST_VIDEO_INFO_FORMAT (out_info) == GST_VIDEO_FORMAT_I420 ||
      GST_VIDEO_INFO_FORMAT (out_info) == GST_VIDEO_FORMAT_I420_10LE);

  /* From/to VUYA */
  if (in_vuya && out_vuya) {
    return setup_convert_info_vuya_to_vuya (self, in_info, out_info);
  } else if (in_vuya) {
    if (out_planar)
      return setup_convert_info_vuya_to_planar (self, in_info, out_info);
    else
      return setup_convert_info_vuya_to_semi_planar (self, in_info, out_info);
  } else if (out_vuya) {
    if (in_planar)
      return setup_convert_info_planar_to_vuya (self, in_info, out_info);
    else if (in_packed)
      return setup_convert_info_packed_yuv_to_vuya (self, in_info, out_info);
    else
      return setup_convert_info_semi_planar_to_vuya (self, in_info, out_info);
  }

  if (in_planar) {
    if (out_planar)
      return setup_convert_info_planar_to_planar (self, in_info, out_info);
    else
      return setup_convert_info_planar_to_semi_planar (self, in_info, out_info);
  } else if (in_packed) {
    if (out_planar)
      return setup_convert_info_packed_yuv_to_planar (self, in_info, out_info);
    else
      return setup_convert_info_packed_yuv_to_semi_planar (self, in_info,
          out_info);
  } else {
    if (out_planar)
      return setup_convert_info_semi_planar_to_planar (self, in_info, out_info);
    else
      return setup_convert_info_semi_planar_to_semi_planar (self, in_info,
          out_info);
  }

  return FALSE;
}

static gboolean
gst_d3d11_color_convert_setup_shader (GstD3D11Converter * self,
    GstD3D11Device * device, GstVideoInfo * in_info, GstVideoInfo * out_info)
{
  HRESULT hr;
  D3D11_SAMPLER_DESC sampler_desc;
  D3D11_INPUT_ELEMENT_DESC input_desc[2];
  D3D11_BUFFER_DESC buffer_desc;
  D3D11_MAPPED_SUBRESOURCE map;
  VertexData *vertex_data;
  WORD *indices;
  ID3D11Device *device_handle;
  ID3D11DeviceContext *context_handle;
  ConvertInfo *convert_info = &self->convert_info;
  /* *INDENT-OFF* */
  ComPtr<ID3D11PixelShader> ps[CONVERTER_MAX_QUADS];
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11InputLayout> layout;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11Buffer> const_buffer;
  ComPtr<ID3D11Buffer> vertex_buffer;
  ComPtr<ID3D11Buffer> index_buffer;
  /* *INDENT-ON* */
  const guint index_count = 2 * 3;
  gint i;
  gboolean ret;

  memset (&sampler_desc, 0, sizeof (sampler_desc));
  memset (input_desc, 0, sizeof (input_desc));
  memset (&buffer_desc, 0, sizeof (buffer_desc));

  device_handle = gst_d3d11_device_get_device_handle (device);
  context_handle = gst_d3d11_device_get_device_context_handle (device);

  /* bilinear filtering */
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
  sampler_desc.MinLOD = 0;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

  hr = device_handle->CreateSamplerState (&sampler_desc, &sampler);
  if (!gst_d3d11_result (hr, device)) {
    GST_ERROR ("Couldn't create sampler state, hr: 0x%x", (guint) hr);
    return FALSE;
  }

  for (i = 0; i < CONVERTER_MAX_QUADS; i++) {
    gchar *shader_code = NULL;

    if (convert_info->ps_body[i]) {
      g_assert (convert_info->ps_output[i] != NULL);

      shader_code = g_strdup_printf (templ_pixel_shader,
          convert_info->templ->constant_buffer ?
          convert_info->templ->constant_buffer : "",
          convert_info->ps_output[i],
          convert_info->templ->func ? convert_info->templ->func : "",
          convert_info->ps_body[i]);

      ret = gst_d3d11_create_pixel_shader (device, shader_code, &ps[i]);
      g_free (shader_code);
      if (!ret) {
        return FALSE;
      }
    }
  }

  if (convert_info->templ->constant_buffer) {
    D3D11_BUFFER_DESC const_buffer_desc = { 0, };

    const_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    const_buffer_desc.ByteWidth = sizeof (PixelShaderColorTransform);
    const_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    const_buffer_desc.MiscFlags = 0;
    const_buffer_desc.StructureByteStride = 0;

    hr = device_handle->CreateBuffer (&const_buffer_desc, NULL, &const_buffer);
    if (!gst_d3d11_result (hr, device)) {
      GST_ERROR ("Couldn't create constant buffer, hr: 0x%x", (guint) hr);
      return FALSE;
    }

    gst_d3d11_device_lock (device);
    hr = context_handle->Map (const_buffer.Get (),
        0, D3D11_MAP_WRITE_DISCARD, 0, &map);

    if (!gst_d3d11_result (hr, device)) {
      GST_ERROR ("Couldn't map constant buffer, hr: 0x%x", (guint) hr);
      gst_d3d11_device_unlock (device);
      return FALSE;
    }

    memcpy (map.pData, &convert_info->transform,
        sizeof (PixelShaderColorTransform));

    context_handle->Unmap (const_buffer.Get (), 0);
    gst_d3d11_device_unlock (device);
  }

  input_desc[0].SemanticName = "POSITION";
  input_desc[0].SemanticIndex = 0;
  input_desc[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
  input_desc[0].InputSlot = 0;
  input_desc[0].AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;
  input_desc[0].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
  input_desc[0].InstanceDataStepRate = 0;

  input_desc[1].SemanticName = "TEXCOORD";
  input_desc[1].SemanticIndex = 0;
  input_desc[1].Format = DXGI_FORMAT_R32G32_FLOAT;
  input_desc[1].InputSlot = 0;
  input_desc[1].AlignedByteOffset = D3D11_APPEND_ALIGNED_ELEMENT;
  input_desc[1].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
  input_desc[1].InstanceDataStepRate = 0;

  if (!gst_d3d11_create_vertex_shader (device, templ_vertex_shader,
          input_desc, G_N_ELEMENTS (input_desc), &vs, &layout)) {
    GST_ERROR ("Couldn't vertex pixel shader");
    return FALSE;
  }

  /* setup vertext buffer and index buffer */
  buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
  buffer_desc.ByteWidth = sizeof (VertexData) * 4;
  buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  hr = device_handle->CreateBuffer (&buffer_desc, NULL, &vertex_buffer);
  if (!gst_d3d11_result (hr, device)) {
    GST_ERROR ("Couldn't create vertex buffer, hr: 0x%x", (guint) hr);
    return FALSE;
  }

  buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
  buffer_desc.ByteWidth = sizeof (WORD) * index_count;
  buffer_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
  buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  hr = device_handle->CreateBuffer (&buffer_desc, NULL, &index_buffer);
  if (!gst_d3d11_result (hr, device)) {
    GST_ERROR ("Couldn't create index buffer, hr: 0x%x", (guint) hr);
    return FALSE;
  }

  gst_d3d11_device_lock (device);
  hr = context_handle->Map (vertex_buffer.Get (), 0, D3D11_MAP_WRITE_DISCARD, 0,
      &map);
  if (!gst_d3d11_result (hr, device)) {
    GST_ERROR ("Couldn't map vertex buffer, hr: 0x%x", (guint) hr);
    gst_d3d11_device_unlock (device);
    return FALSE;
  }

  vertex_data = (VertexData *) map.pData;

  hr = context_handle->Map (index_buffer.Get (), 0, D3D11_MAP_WRITE_DISCARD, 0,
      &map);
  if (!gst_d3d11_result (hr, device)) {
    GST_ERROR ("Couldn't map index buffer, hr: 0x%x", (guint) hr);
    context_handle->Unmap (vertex_buffer.Get (), 0);
    gst_d3d11_device_unlock (device);
    return FALSE;
  }

  indices = (WORD *) map.pData;

  /* bottom left */
  vertex_data[0].position.x = -1.0f;
  vertex_data[0].position.y = -1.0f;
  vertex_data[0].position.z = 0.0f;
  vertex_data[0].texture.x = 0.0f;
  vertex_data[0].texture.y = 1.0f;

  /* top left */
  vertex_data[1].position.x = -1.0f;
  vertex_data[1].position.y = 1.0f;
  vertex_data[1].position.z = 0.0f;
  vertex_data[1].texture.x = 0.0f;
  vertex_data[1].texture.y = 0.0f;

  /* top right */
  vertex_data[2].position.x = 1.0f;
  vertex_data[2].position.y = 1.0f;
  vertex_data[2].position.z = 0.0f;
  vertex_data[2].texture.x = 1.0f;
  vertex_data[2].texture.y = 0.0f;

  /* bottom right */
  vertex_data[3].position.x = 1.0f;
  vertex_data[3].position.y = -1.0f;
  vertex_data[3].position.z = 0.0f;
  vertex_data[3].texture.x = 1.0f;
  vertex_data[3].texture.y = 1.0f;

  /* clockwise indexing */
  indices[0] = 0;               /* bottom left */
  indices[1] = 1;               /* top left */
  indices[2] = 2;               /* top right */

  indices[3] = 3;               /* bottom right */
  indices[4] = 0;               /* bottom left  */
  indices[5] = 2;               /* top right */

  context_handle->Unmap (vertex_buffer.Get (), 0);
  context_handle->Unmap (index_buffer.Get (), 0);
  gst_d3d11_device_unlock (device);

  self->quad[0] = gst_d3d11_quad_new (device,
      ps[0].Get (), vs.Get (), layout.Get (), sampler.Get (), NULL, NULL,
      const_buffer.Get (), vertex_buffer.Get (), sizeof (VertexData),
      index_buffer.Get (), DXGI_FORMAT_R16_UINT, index_count);

  if (ps[1]) {
    self->quad[1] = gst_d3d11_quad_new (device,
        ps[1].Get (), vs.Get (), layout.Get (), sampler.Get (), NULL, NULL,
        const_buffer.Get (), vertex_buffer.Get (), sizeof (VertexData),
        index_buffer.Get (), DXGI_FORMAT_R16_UINT, index_count);
  }

  self->num_input_view = GST_VIDEO_INFO_N_PLANES (in_info);
  self->num_output_view = GST_VIDEO_INFO_N_PLANES (out_info);

  /* holds vertex buffer for crop rect update */
  self->vertex_buffer = vertex_buffer.Detach ();

  self->src_rect.left = 0;
  self->src_rect.top = 0;
  self->src_rect.right = GST_VIDEO_INFO_WIDTH (in_info);
  self->src_rect.bottom = GST_VIDEO_INFO_HEIGHT (in_info);

  self->dest_rect.left = 0;
  self->dest_rect.top = 0;
  self->dest_rect.right = GST_VIDEO_INFO_WIDTH (out_info);
  self->dest_rect.bottom = GST_VIDEO_INFO_HEIGHT (out_info);

  self->input_texture_width = GST_VIDEO_INFO_WIDTH (in_info);
  self->input_texture_height = GST_VIDEO_INFO_HEIGHT (in_info);

  return TRUE;
}

static GstD3D11Converter *
gst_d3d11_converter_new_internal (GstD3D11Device * device,
    GstVideoInfo * in_info, GstVideoInfo * out_info, gfloat alpha)
{
  const GstVideoInfo *unknown_info;
  const GstD3D11Format *in_d3d11_format;
  const GstD3D11Format *out_d3d11_format;
  gboolean is_supported = FALSE;
  MatrixData matrix;
  GstD3D11Converter *converter = NULL;
  gboolean ret;
  guint i;

  g_return_val_if_fail (GST_IS_D3D11_DEVICE (device), NULL);
  g_return_val_if_fail (in_info != NULL, NULL);
  g_return_val_if_fail (out_info != NULL, NULL);

  GST_DEBUG ("Setup convert with format %s -> %s",
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (in_info)),
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (out_info)));

  in_d3d11_format =
      gst_d3d11_device_format_from_gst (device,
      GST_VIDEO_INFO_FORMAT (in_info));
  if (!in_d3d11_format) {
    unknown_info = in_info;
    goto format_unknown;
  }

  out_d3d11_format =
      gst_d3d11_device_format_from_gst (device,
      GST_VIDEO_INFO_FORMAT (out_info));
  if (!out_d3d11_format) {
    unknown_info = out_info;
    goto format_unknown;
  }

  converter = g_new0 (GstD3D11Converter, 1);
  converter->device = (GstD3D11Device *) gst_object_ref (device);
  converter->alpha = alpha;

  if (GST_VIDEO_INFO_IS_RGB (in_info)) {
    if (GST_VIDEO_INFO_IS_RGB (out_info)) {
      is_supported =
          setup_convert_info_rgb_to_rgb (converter, in_info, out_info);
    } else if (GST_VIDEO_INFO_IS_YUV (out_info)) {
      is_supported =
          setup_convert_info_rgb_to_yuv (converter, in_info, out_info);
    }
  } else if (GST_VIDEO_INFO_IS_YUV (in_info)) {
    if (GST_VIDEO_INFO_IS_RGB (out_info)) {
      is_supported =
          setup_convert_info_yuv_to_rgb (converter, in_info, out_info);
    } else if (GST_VIDEO_INFO_IS_YUV (out_info)) {
      is_supported =
          setup_convert_info_yuv_to_yuv (converter, in_info, out_info);
    }
  }

  if (!is_supported) {
    goto conversion_not_supported;
  }

  if (converter_get_matrix (converter, &matrix, in_info, out_info)) {
    PixelShaderColorTransform *transform = &converter->convert_info.transform;

    /* padding the last column for 16bytes alignment */
    transform->trans_matrix[0] = matrix.dm[0][0];
    transform->trans_matrix[1] = matrix.dm[0][1];
    transform->trans_matrix[2] = matrix.dm[0][2];
    transform->trans_matrix[3] = 0;
    transform->trans_matrix[4] = matrix.dm[1][0];
    transform->trans_matrix[5] = matrix.dm[1][1];
    transform->trans_matrix[6] = matrix.dm[1][2];
    transform->trans_matrix[7] = 0;
    transform->trans_matrix[8] = matrix.dm[2][0];
    transform->trans_matrix[9] = matrix.dm[2][1];
    transform->trans_matrix[10] = matrix.dm[2][2];
    transform->trans_matrix[11] = 0;
  }

  for (i = 0; i < GST_VIDEO_INFO_N_PLANES (out_info); i++) {
    converter->viewport[i].TopLeftX = 0;
    converter->viewport[i].TopLeftY = 0;
    converter->viewport[i].Width = GST_VIDEO_INFO_COMP_WIDTH (out_info, i);
    converter->viewport[i].Height = GST_VIDEO_INFO_COMP_HEIGHT (out_info, i);
    converter->viewport[i].MinDepth = 0.0f;
    converter->viewport[i].MaxDepth = 1.0f;
  }

  ret = gst_d3d11_color_convert_setup_shader (converter,
      device, in_info, out_info);

  if (!ret) {
    GST_ERROR ("Couldn't setup shader");
    gst_d3d11_converter_free (converter);
    converter = NULL;
  } else {
    converter->in_info = *in_info;
    converter->out_info = *out_info;
  }

  return converter;

  /* ERRORS */
format_unknown:
  {
    GST_ERROR ("%s couldn't be converted to d3d11 format",
        gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (unknown_info)));
    return NULL;
  }
conversion_not_supported:
  {
    GST_ERROR ("Conversion %s to %s not supported",
        gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (in_info)),
        gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (out_info)));
    gst_d3d11_converter_free (converter);
    return NULL;
  }
}

GstD3D11Converter *
gst_d3d11_converter_new (GstD3D11Device * device,
    GstVideoInfo * in_info, GstVideoInfo * out_info)
{
  return gst_d3d11_converter_new_internal (device, in_info, out_info, 1.0f);
}

GstD3D11Converter *
gst_d3d11_converter_new_with_alpha (GstD3D11Device * device,
    GstVideoInfo * in_info, GstVideoInfo * out_info, gfloat alpha)
{
  g_return_val_if_fail (alpha >= 0.0f, NULL);
  g_return_val_if_fail (alpha <= 1.0f, NULL);

  return gst_d3d11_converter_new_internal (device, in_info, out_info, alpha);
}

void
gst_d3d11_converter_free (GstD3D11Converter * converter)
{
  gint i;

  g_return_if_fail (converter != NULL);

  for (i = 0; i < CONVERTER_MAX_QUADS; i++) {
    if (converter->quad[i])
      gst_d3d11_quad_free (converter->quad[i]);

    g_free (converter->convert_info.ps_body[i]);
  }

  GST_D3D11_CLEAR_COM (converter->vertex_buffer);

  gst_clear_object (&converter->device);
  g_free (converter);
}

/* must be called with gst_d3d11_device_lock since ID3D11DeviceContext is not
 * thread-safe */
static gboolean
gst_d3d11_converter_update_vertex_buffer (GstD3D11Converter * self)
{
  D3D11_MAPPED_SUBRESOURCE map;
  VertexData *vertex_data;
  ID3D11DeviceContext *context_handle;
  HRESULT hr;
  FLOAT x1, y1, x2, y2;
  FLOAT u, v;
  const RECT *src_rect = &self->src_rect;
  const RECT *dest_rect = &self->dest_rect;
  gint texture_width = self->input_texture_width;
  gint texture_height = self->input_texture_height;
  gdouble val;

  context_handle = gst_d3d11_device_get_device_context_handle (self->device);

  hr = context_handle->Map (self->vertex_buffer, 0, D3D11_MAP_WRITE_DISCARD,
      0, &map);

  if (!gst_d3d11_result (hr, self->device)) {
    GST_ERROR ("Couldn't map vertex buffer, hr: 0x%x", (guint) hr);
    return FALSE;
  }

  vertex_data = (VertexData *) map.pData;
  /* bottom left */
  gst_util_fraction_to_double (dest_rect->left,
      GST_VIDEO_INFO_WIDTH (&self->out_info), &val);
  x1 = (val * 2.0f) - 1.0f;

  gst_util_fraction_to_double (dest_rect->bottom,
      GST_VIDEO_INFO_HEIGHT (&self->out_info), &val);
  y1 = (val * -2.0f) + 1.0f;

  /* top right */
  gst_util_fraction_to_double (dest_rect->right,
      GST_VIDEO_INFO_WIDTH (&self->out_info), &val);
  x2 = (val * 2.0f) - 1.0f;

  gst_util_fraction_to_double (dest_rect->top,
      GST_VIDEO_INFO_HEIGHT (&self->out_info), &val);
  y2 = (val * -2.0f) + 1.0f;

  /* bottom left */
  u = (src_rect->left / (gfloat) texture_width) - 0.5f / texture_width;
  v = (src_rect->bottom / (gfloat) texture_height) - 0.5f / texture_height;

  vertex_data[0].position.x = x1;
  vertex_data[0].position.y = y1;
  vertex_data[0].position.z = 0.0f;
  vertex_data[0].texture.x = u;
  vertex_data[0].texture.y = v;

  /* top left */
  u = (src_rect->left / (gfloat) texture_width) - 0.5f / texture_width;
  v = (src_rect->top / (gfloat) texture_height) - 0.5f / texture_height;

  vertex_data[1].position.x = x1;
  vertex_data[1].position.y = y2;
  vertex_data[1].position.z = 0.0f;
  vertex_data[1].texture.x = u;
  vertex_data[1].texture.y = v;

  /* top right */
  u = (src_rect->right / (gfloat) texture_width) - 0.5f / texture_width;
  v = (src_rect->top / (gfloat) texture_height) - 0.5f / texture_height;

  vertex_data[2].position.x = x2;
  vertex_data[2].position.y = y2;
  vertex_data[2].position.z = 0.0f;
  vertex_data[2].texture.x = u;
  vertex_data[2].texture.y = v;

  /* bottom right */
  u = (src_rect->right / (gfloat) texture_width) - 0.5f / texture_width;
  v = (src_rect->bottom / (gfloat) texture_height) - 0.5f / texture_height;

  vertex_data[3].position.x = x2;
  vertex_data[3].position.y = y1;
  vertex_data[3].position.z = 0.0f;
  vertex_data[3].texture.x = u;
  vertex_data[3].texture.y = v;

  context_handle->Unmap (self->vertex_buffer, 0);

  self->update_vertex = FALSE;

  return TRUE;
}

gboolean
gst_d3d11_converter_convert (GstD3D11Converter * converter,
    ID3D11ShaderResourceView * srv[GST_VIDEO_MAX_PLANES],
    ID3D11RenderTargetView * rtv[GST_VIDEO_MAX_PLANES],
    ID3D11BlendState * blend, gfloat blend_factor[4])
{
  gboolean ret;

  g_return_val_if_fail (converter != NULL, FALSE);
  g_return_val_if_fail (srv != NULL, FALSE);
  g_return_val_if_fail (rtv != NULL, FALSE);

  gst_d3d11_device_lock (converter->device);
  ret = gst_d3d11_converter_convert_unlocked (converter,
      srv, rtv, blend, blend_factor);
  gst_d3d11_device_unlock (converter->device);

  return ret;
}

gboolean
gst_d3d11_converter_convert_unlocked (GstD3D11Converter * converter,
    ID3D11ShaderResourceView * srv[GST_VIDEO_MAX_PLANES],
    ID3D11RenderTargetView * rtv[GST_VIDEO_MAX_PLANES],
    ID3D11BlendState * blend, gfloat blend_factor[4])
{
  gboolean ret;
  /* *INDENT-OFF* */
  ComPtr<ID3D11Resource> resource;
  ComPtr<ID3D11Texture2D> texture;
  /* *INDENT-ON* */
  D3D11_TEXTURE2D_DESC desc;

  g_return_val_if_fail (converter != NULL, FALSE);
  g_return_val_if_fail (srv != NULL, FALSE);
  g_return_val_if_fail (rtv != NULL, FALSE);

  /* check texture resolution and update crop area */
  srv[0]->GetResource (&resource);
  resource.As (&texture);
  texture->GetDesc (&desc);

  if (converter->update_vertex ||
      desc.Width != (guint) converter->input_texture_width ||
      desc.Height != (guint) converter->input_texture_height) {
    GST_DEBUG ("Update vertext buffer, texture resolution: %dx%d",
        desc.Width, desc.Height);

    converter->input_texture_width = desc.Width;
    converter->input_texture_height = desc.Height;

    if (!gst_d3d11_converter_update_vertex_buffer (converter)) {
      GST_ERROR ("Cannot update vertex buffer");
      return FALSE;
    }
  }

  ret = gst_d3d11_draw_quad_unlocked (converter->quad[0], converter->viewport,
      1, srv, converter->num_input_view, rtv, 1, NULL, blend, blend_factor);

  if (!ret)
    return FALSE;

  if (converter->quad[1]) {
    ret = gst_d3d11_draw_quad_unlocked (converter->quad[1],
        &converter->viewport[1], converter->num_output_view - 1,
        srv, converter->num_input_view, &rtv[1], converter->num_output_view - 1,
        NULL, blend, blend_factor);

    if (!ret)
      return FALSE;
  }

  return TRUE;
}

gboolean
gst_d3d11_converter_update_viewport (GstD3D11Converter * converter,
    D3D11_VIEWPORT * viewport)
{
  g_return_val_if_fail (converter != NULL, FALSE);
  g_return_val_if_fail (viewport != NULL, FALSE);

  converter->viewport[0] = *viewport;

  switch (GST_VIDEO_INFO_FORMAT (&converter->out_info)) {
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_P010_10LE:
    case GST_VIDEO_FORMAT_P016_LE:
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_I420_10LE:{
      guint i;
      converter->viewport[1].TopLeftX = converter->viewport[0].TopLeftX / 2;
      converter->viewport[1].TopLeftY = converter->viewport[0].TopLeftY / 2;
      converter->viewport[1].Width = converter->viewport[0].Width / 2;
      converter->viewport[1].Height = converter->viewport[0].Height / 2;

      for (i = 2; i < GST_VIDEO_INFO_N_PLANES (&converter->out_info); i++)
        converter->viewport[i] = converter->viewport[1];

      break;
    }
    default:
      if (converter->num_output_view > 1)
        g_assert_not_reached ();
      break;
  }

  return TRUE;
}

gboolean
gst_d3d11_converter_update_src_rect (GstD3D11Converter * converter,
    RECT * src_rect)
{
  g_return_val_if_fail (converter != NULL, FALSE);
  g_return_val_if_fail (src_rect != NULL, FALSE);

  if (converter->src_rect.left != src_rect->left ||
      converter->src_rect.top != src_rect->top ||
      converter->src_rect.right != src_rect->right ||
      converter->src_rect.bottom != src_rect->bottom) {
    converter->src_rect = *src_rect;

    /* vertex buffer will be updated on next convert() call */
    converter->update_vertex = TRUE;
  }

  return TRUE;
}

gboolean
gst_d3d11_converter_update_dest_rect (GstD3D11Converter * converter,
    RECT * dest_rect)
{
  g_return_val_if_fail (converter != NULL, FALSE);
  g_return_val_if_fail (dest_rect != NULL, FALSE);

  if (converter->dest_rect.left != dest_rect->left ||
      converter->dest_rect.top != dest_rect->top ||
      converter->dest_rect.right != dest_rect->right ||
      converter->dest_rect.bottom != dest_rect->bottom) {
    converter->dest_rect = *dest_rect;

    /* vertex buffer will be updated on next convert() call */
    converter->update_vertex = TRUE;
  }

  return TRUE;
}
