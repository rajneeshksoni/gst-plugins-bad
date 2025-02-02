/* GStreamer
 * Copyright (C) 2020 Seungha Yang <seungha@centricular.com>
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

#ifndef __GST_WASAPI2_UTIL_H__
#define __GST_WASAPI2_UTIL_H__

#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <windows.h>
#include <audioclient.h>

G_BEGIN_DECLS

/* Static Caps shared between source, sink, and device provider */
#define GST_WASAPI2_STATIC_CAPS "audio/x-raw, " \
        "format = (string) " GST_AUDIO_FORMATS_ALL ", " \
        "layout = (string) interleaved, " \
        "rate = " GST_AUDIO_RATE_RANGE ", " \
        "channels = " GST_AUDIO_CHANNELS_RANGE

gboolean  _gst_wasapi2_result (HRESULT hr,
                               GstDebugCategory * cat,
                               const gchar * file,
                               const gchar * function,
                               gint line);

#define gst_wasapi2_result(result) \
    _gst_wasapi2_result (result, GST_CAT_DEFAULT, __FILE__, GST_FUNCTION, __LINE__)

G_END_DECLS

#endif /* __GST_WASAPI_UTIL_H__ */
