/* GStreamer
 * Copyright (C) 2008 Wim Taymans <wim.taymans at gmail.com>
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

#include <gst/gst.h>

#include <gst/rtsp-server/rtsp-server-wfd.h>
#include <gst/rtsp-server/rtsp-media-factory-wfd.h>

#if 0
#define VIDEO_PIPELINE "ximagesrc ! videorate ! videoscale ! videoconvert ! " \
  "video/x-raw,width=640,height=480,framerate=30/1 ! " \
  "x264enc aud=false byte-stream=true bitrate=512 ! video/x-h264,profile=baseline ! mpegtsmux wfd-mode=TRUE ! " \
  "rtpmp2tpay name=pay0 pt=33"
#define AUDIO_PIPELINE "pulsesrc device=alsa_output.pci-0000_00_1b.0.analog-stereo.monitor ! audioconvert ! " \
  "faac ! mpegtsmux wfd-mode=TRUE ! " \
  "rtpmp2tpay name=pay0 pt=33"
#else
#define VIDEO_PIPELINE "ximagesrc ! videoscale ! videoconvert ! " \
  "video/x-raw,width=640,height=480,framerate=60/1 ! " \
  "x264enc aud=false byte-stream=true bitrate=512 ! video/x-h264,profile=baseline ! mpegtsmux name=mux " \
  "pulsesrc device=alsa_output.pci-0000_00_1b.0.analog-stereo.monitor ! audioconvert ! " \
  "faac ! mux. mux. ! " \
  "rtpmp2tpay name=pay0 pt=33"
#if 0
#define VIDEO_PIPELINE "ximagesrc startx=0 starty=0 endx=1919 endy=1079 ! videorate ! videoscale ! video/x-raw,width=1280,height=720,framerate=30/1 ! videoconvert ! " \
  "x264enc aud=false byte-stream=true bitrate=512 ! video/x-h264,profile=baseline ! mpegtsmux name=mux wfd-mode=TRUE " \
  "pulsesrc device=alsa_output.pci-0000_00_1b.0.analog-stereo.monitor ! audioconvert ! " \
  "faac ! mux. mux. ! " \
  "rtpmp2tpay name=pay0 pt=33"
#endif
/*
#define VIDEO_PIPELINE "ximagesrc do-timestamp=true ! videoscale ! video/x-raw,width=1280,height=720,framerate=30/1 ! videoconvert ! " \
  "x264enc aud=false byte-stream=true bitrate=512 ! video/x-h264,profile=baseline ! mpegtsmux name=mux wfd-mode=TRUE " \
  "pulsesrc device=alsa_output.pci-0000_00_1b.0.analog-stereo.monitor provide-clock=false ! audioconvert ! " \
  "faac ! mux. mux. ! " \
  "rtpmp2tpay name=pay0 pt=33"
*/
#endif

#define WFD_RTSP_PORT "2022"
#define TEST_MOUNT_POINT  "/wfd1.0/streamid=0"

static gboolean
timeout (GMainLoop * loop, gboolean ignored)
{
  g_main_loop_quit (loop);
  return FALSE;
}

int main (int argc, char *argv[])
{
  GMainLoop *loop;
  GstRTSPWFDServer *server;
  guint id;
  GstRTSPMountPoints *mounts;
  GstRTSPMediaFactoryWFD *factory;


  gst_init (&argc, &argv);

  loop = g_main_loop_new (NULL, FALSE);

  /* create a server instance */
  server = gst_rtsp_wfd_server_new ();

  gst_rtsp_server_set_address(GST_RTSP_SERVER(server), "192.168.3.100"); 
  gst_rtsp_server_set_service(GST_RTSP_SERVER(server), WFD_RTSP_PORT);
  mounts = gst_rtsp_server_get_mount_points (GST_RTSP_SERVER(server));

  factory = gst_rtsp_media_factory_wfd_new ();

  gst_rtsp_media_factory_set_launch (GST_RTSP_MEDIA_FACTORY(factory),
      "( " VIDEO_PIPELINE " )");
  gst_rtsp_mount_points_add_factory (mounts, TEST_MOUNT_POINT, GST_RTSP_MEDIA_FACTORY(factory));
  g_object_unref (mounts);

  /* attach the server to the default maincontext */
  if ((id = gst_rtsp_server_attach (GST_RTSP_SERVER_CAST(server), NULL)) == 0)
    goto failed;

  g_timeout_add_seconds (1000, (GSourceFunc) timeout, loop);

  /* start serving */
  g_main_loop_run (loop);

  /* cleanup */
  g_source_remove (id);
  g_object_unref (server);
  g_main_loop_unref (loop);

  return 0;

  /* ERRORS */
failed:
  {
    g_print ("failed to attach the server\n");
    return -1;
  }
}
