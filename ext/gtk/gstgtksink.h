/*
 * GStreamer
 * Copyright (C) 2015 Matthew Waters <matthew@centricular.com>
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

#ifndef __GST_GTK_SINK_H__
#define __GST_GTK_SINK_H__

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/video.h>

typedef struct _GstGtkSink GstGtkSink;
typedef struct _GstGtkSinkClass GstGtkSinkClass;

#include <gtkgstwidget.h>

G_BEGIN_DECLS

GType gst_gtk_sink_get_type (void);
#define GST_TYPE_GTK_SINK            (gst_gtk_sink_get_type())
#define GST_GTK_SINK(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_GTK_SINK,GstGtkSink))
#define GST_GTK_SINK_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_GTK_SINK,GstGtkSinkClass))
#define GST_IS_GTK_SINK(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_GTK_SINK))
#define GST_IS_GTK_SINK_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_GTK_SINK))
#define GST_GTK_SINK_CAST(obj)       ((GstGtkSink*)(obj))

/**
 * GstGtkSink:
 *
 * Opaque #GstGtkSink object
 */
struct _GstGtkSink
{
  /* <private> */
  GstVideoSink         parent;

  GstVideoInfo         v_info;

  GtkGstWidget        *widget;

  /* properties */
  gboolean             force_aspect_ratio;
  GBinding             *bind_aspect_ratio;

  gint                  par_n;
  gint                  par_d;
  GBinding             *bind_pixel_aspect_ratio;

  gboolean              ignore_alpha;
  GBinding             *bind_ignore_alpha;
};

/**
 * GstGtkSinkClass:
 *
 * The #GstGtkSinkClass struct only contains private data
 */
struct _GstGtkSinkClass
{
  /* <private> */
  GstVideoSinkClass object_class;
};

GstGtkSink *    gst_gtk_sink_new (void);

G_END_DECLS

#endif /* __GST_GTK_SINK_H__ */
