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
/**
 * SECTION:rtsp-client
 * @short_description: A client connection state
 * @see_also: #GstRTSPServer, #GstRTSPThreadPool
 *
 * The client object handles the connection with a client for as long as a TCP
 * connection is open.
 *
 * A #GstRTSPWFDClient is created by #GstRTSPServer when a new connection is
 * accepted and it inherits the #GstRTSPMountPoints, #GstRTSPSessionPool,
 * #GstRTSPAuth and #GstRTSPThreadPool from the server.
 *
 * The client connection should be configured with the #GstRTSPConnection using
 * gst_rtsp_wfd_client_set_connection() before it can be attached to a #GMainContext
 * using gst_rtsp_wfd_client_attach(). From then on the client will handle requests
 * on the connection.
 *
 * Use gst_rtsp_wfd_client_session_filter() to iterate or modify all the
 * #GstRTSPSession objects managed by the client object.
 *
 * Last reviewed on 2013-07-11 (1.0.0)
 */

#include <stdio.h>
#include <string.h>

#include "rtsp-client-wfd.h"
#include "rtsp-sdp.h"
#include "rtsp-params.h"
#include "gstwfdmessage.h"

#define GST_RTSP_WFD_CLIENT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_RTSP_WFD_CLIENT, GstRTSPWFDClientPrivate))

/* locking order:
 * send_lock, lock, tunnels_lock
 */

struct _GstRTSPWFDClientPrivate
{
  GstRTSPWFDClientSendFunc send_func;   /* protected by send_lock */
  gpointer send_data;           /* protected by send_lock */
  GDestroyNotify send_notify;   /* protected by send_lock */

  /* used to cache the media in the last requested DESCRIBE so that
   * we can pick it up in the next SETUP immediately */
  gchar *path;
  GstRTSPMedia *media;

  GList *transports;
  GList *sessions;

  guint8 m1_done;
  guint8 m3_done;
  guint8 m4_done;

  /* Parameters for WIFI-DISPLAY */
  guint caCodec;
  guint cFreq;
  guint cChanels;
  guint cBitwidth;
  guint caLatency;
  guint cvCodec;
  guint cNative;
  guint64 cNativeResolution;
  guint64 cVideo_reso_supported;
  gint cSrcNative;
  guint cCEAResolution;
  guint cVESAResolution;
  guint cHHResolution;
  guint cProfile;
  guint cLevel;
  guint32 cMaxHeight;
  guint32 cMaxWidth;
  guint32 cFramerate;
  guint32 cInterleaved;
  guint32 cmin_slice_size;
  guint32 cslice_enc_params;
  guint cframe_rate_control;
  guint cvLatency;
  guint ctrans;
  guint cprofile;
  guint clowertrans;
  guint32 crtp_port0;
  guint32 crtp_port1;

  gboolean protection_enabled;
  GstWFDHDCPProtection hdcp_version;
  guint32 hdcp_tcpport;

  gboolean edid_supported;
  guint32 edid_hres;
  guint32 edid_vres;
};

#define DEFAULT_WFD_TIMEOUT 60

enum
{
  SIGNAL_WFD_OPTIONS_REQUEST,
  SIGNAL_WFD_GET_PARAMETER_REQUEST,
  SIGNAL_WFD_LAST
};

GST_DEBUG_CATEGORY_STATIC (rtsp_wfd_client_debug);
#define GST_CAT_DEFAULT rtsp_wfd_client_debug

static guint gst_rtsp_client_wfd_signals[SIGNAL_WFD_LAST] = { 0 };

static void gst_rtsp_wfd_client_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec);
static void gst_rtsp_wfd_client_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec);
static void gst_rtsp_wfd_client_finalize (GObject * obj);

static gboolean handle_wfd_options_request (GstRTSPClient * client,
    GstRTSPContext * ctx);
static gboolean handle_wfd_set_param_request (GstRTSPClient * client,
    GstRTSPContext * ctx);
static gboolean handle_wfd_get_param_request (GstRTSPClient * client,
    GstRTSPContext * ctx);

static void send_generic_wfd_response (GstRTSPWFDClient * client,
    GstRTSPStatusCode code, GstRTSPContext * ctx);
static gchar *wfd_make_path_from_uri (GstRTSPClient * client,
    const GstRTSPUrl * uri);
static void wfd_options_request_done (GstRTSPWFDClient * client);
static void wfd_get_param_request_done (GstRTSPWFDClient * client);
static void handle_wfd_response (GstRTSPClient * client, GstRTSPContext * ctx);

GstRTSPResult prepare_trigger_request (GstRTSPWFDClient * client,
    GstRTSPMessage * request, GstWFDTriggerType trigger_type, gchar * url);

GstRTSPResult prepare_request (GstRTSPWFDClient * client,
    GstRTSPMessage * request, GstRTSPMethod method, gchar * url);

void
send_request (GstRTSPWFDClient * client, GstRTSPSession * session,
    GstRTSPMessage * request);

GstRTSPResult
prepare_response (GstRTSPWFDClient * client, GstRTSPMessage * request,
    GstRTSPMessage * response, GstRTSPMethod method);

static GstRTSPResult handle_M1_message (GstRTSPWFDClient * client);
static GstRTSPResult handle_M3_message (GstRTSPWFDClient * client);
static GstRTSPResult handle_M4_message (GstRTSPWFDClient * client);

G_DEFINE_TYPE (GstRTSPWFDClient, gst_rtsp_wfd_client, GST_TYPE_RTSP_CLIENT);

static void
gst_rtsp_wfd_client_class_init (GstRTSPWFDClientClass * klass)
{
  GObjectClass *gobject_class;
  GstRTSPClientClass *rtsp_client_class;

  g_type_class_add_private (klass, sizeof (GstRTSPWFDClientPrivate));

  gobject_class = G_OBJECT_CLASS (klass);
  rtsp_client_class = GST_RTSP_CLIENT_CLASS (klass);

  gobject_class->get_property = gst_rtsp_wfd_client_get_property;
  gobject_class->set_property = gst_rtsp_wfd_client_set_property;
  gobject_class->finalize = gst_rtsp_wfd_client_finalize;

  //klass->create_sdp = create_sdp;
  //klass->configure_client_media = default_configure_client_media;
  //klass->configure_client_transport = default_configure_client_transport;
  //klass->params_set = default_params_set;
  //klass->params_get = default_params_get;

  rtsp_client_class->handle_options_request = handle_wfd_options_request;
  rtsp_client_class->handle_set_param_request = handle_wfd_set_param_request;
  rtsp_client_class->handle_get_param_request = handle_wfd_get_param_request;
  rtsp_client_class->make_path_from_uri = wfd_make_path_from_uri;

  rtsp_client_class->handle_response = handle_wfd_response;

  gst_rtsp_client_wfd_signals[SIGNAL_WFD_OPTIONS_REQUEST] =
      g_signal_new ("wfd-options-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPWFDClientClass,
          wfd_options_request), NULL, NULL, g_cclosure_marshal_VOID__POINTER,
      G_TYPE_NONE, 1, G_TYPE_POINTER);

  gst_rtsp_client_wfd_signals[SIGNAL_WFD_GET_PARAMETER_REQUEST] =
      g_signal_new ("wfd-get-parameter-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPWFDClientClass,
          wfd_get_param_request), NULL, NULL, g_cclosure_marshal_VOID__POINTER,
      G_TYPE_NONE, 1, G_TYPE_POINTER);

  klass->wfd_options_request = wfd_options_request_done;
  klass->wfd_get_param_request = wfd_get_param_request_done;

  GST_DEBUG_CATEGORY_INIT (rtsp_wfd_client_debug, "rtspwfdclient", 0,
      "GstRTSPWFDClient");
}

static void
gst_rtsp_wfd_client_init (GstRTSPWFDClient * client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  client->priv = priv;
  priv->protection_enabled = FALSE;
  GST_INFO_OBJECT (client, "Client is initialized");
}

/* A client is finalized when the connection is broken */
static void
gst_rtsp_wfd_client_finalize (GObject * obj)
{
  GstRTSPWFDClient *client = GST_RTSP_WFD_CLIENT (obj);
  //GstRTSPWFDClientPrivate *priv = client->priv;

  GST_INFO ("finalize client %p", client);

  G_OBJECT_CLASS (gst_rtsp_wfd_client_parent_class)->finalize (obj);
}

static void
gst_rtsp_wfd_client_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec)
{
  //GstRTSPWFDClient *client = GST_RTSP_WFD_CLIENT (object);

  switch (propid) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

static void
gst_rtsp_wfd_client_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec)
{
  //GstRTSPWFDClient *client = GST_RTSP_WFD_CLIENT (object);

  switch (propid) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

/**
 * gst_rtsp_wfd_client_new:
 *
 * Create a new #GstRTSPWFDClient instance.
 *
 * Returns: a new #GstRTSPWFDClient
 */
GstRTSPWFDClient *
gst_rtsp_wfd_client_new (void)
{
  GstRTSPWFDClient *result;

  result = g_object_new (GST_TYPE_RTSP_WFD_CLIENT, NULL);

  return result;
}

void
gst_rtsp_wfd_client_start_wfd (GstRTSPWFDClient * client)
{
  GstRTSPResult res = GST_RTSP_OK;
  GST_INFO_OBJECT (client, "gst_rtsp_wfd_client_start_wfd");

  res = handle_M1_message (client);
  if (res < GST_RTSP_OK) {
    GST_ERROR_OBJECT (client, "handle_M1_message failed : %d", res);
    goto error;
  }

error:
  return;
}

gboolean g_m1_done = FALSE;
gboolean g_m4_done = FALSE;

static void
wfd_options_request_done (GstRTSPWFDClient * client)
{
  GST_INFO_OBJECT (client, "M2 done..");

  handle_M3_message (client);
}

static void
wfd_get_param_request_done (GstRTSPWFDClient * client)
{
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);
  priv->m3_done = TRUE;
  GST_INFO_OBJECT (client, "M3 done..");
  handle_M4_message (client);
}

static gchar *
wfd_make_path_from_uri (GstRTSPClient * client, const GstRTSPUrl * uri)
{
  gchar *path;

  GST_DEBUG_OBJECT (client, "Got URI abspath : %s", uri->abspath);
  path = g_strdup ("/wfd1.0/streamid=0");

  return path;
}

static void
handle_wfd_response (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPResult res = GST_RTSP_OK;
  guint8 *data = NULL;
  guint size = 0;

  GstRTSPWFDClient *_client = GST_RTSP_WFD_CLIENT (client);
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  GST_INFO_OBJECT (_client, "Handling response..");

  if (!ctx)
    GST_ERROR_OBJECT (_client, "Context is NULL");
  if (!ctx->response)
    GST_ERROR_OBJECT (_client, "Response is NULL");

  /* parsing the GET_PARAMTER response */
  res = gst_rtsp_message_get_body (ctx->response, (guint8 **) & data, &size);
  if (res != GST_RTSP_OK) {
    GST_ERROR_OBJECT (_client, "Failed to get body of response...");
    return;
  }

  GST_INFO_OBJECT (_client, "Response body is %d", size);
  if (size > 0) {
    if (!priv->m3_done) {
      GstWFDResult wfd_res;
      GstWFDMessage *msg = NULL;
      /* Parse M3 response from sink */
      wfd_res = gst_wfd_message_new (&msg);
      if (wfd_res != GST_WFD_OK) {
        GST_ERROR_OBJECT (client, "Failed to create wfd message...");
        goto error;
      }

      wfd_res = gst_wfd_message_init (msg);
      if (wfd_res != GST_WFD_OK) {
        GST_ERROR_OBJECT (client, "Failed to init wfd message...");
        goto error;
      }

      wfd_res = gst_wfd_message_parse_buffer (data, size, msg);

      GST_DEBUG_OBJECT (client, "M3 response server side message body: %s",
          gst_wfd_message_as_text (msg));

      /* Get the audio formats supported by WFDSink */
      if (msg->audio_codecs) {
        wfd_res =
            gst_wfd_message_get_supported_audio_format (msg, &priv->caCodec,
            &priv->cFreq, &priv->cChanels, &priv->cBitwidth, &priv->caLatency);
        if (wfd_res != GST_WFD_OK) {
          GST_WARNING_OBJECT (client,
              "Failed to get wfd support audio formats...");
          goto error;
        }
      }

      /* Get the Video formats supported by WFDSink */
      wfd_res =
          gst_wfd_message_get_supported_video_format (msg, &priv->cvCodec,
          &priv->cNative, &priv->cNativeResolution,
          (guint64 *) & priv->cCEAResolution,
          (guint64 *) & priv->cVESAResolution,
          (guint64 *) & priv->cHHResolution, &priv->cProfile, &priv->cLevel,
          &priv->cvLatency, &priv->cMaxHeight, &priv->cMaxWidth,
          &priv->cmin_slice_size, &priv->cslice_enc_params,
          &priv->cframe_rate_control);
      if (wfd_res != GST_WFD_OK) {
        GST_WARNING_OBJECT (client,
            "Failed to get wfd supported video formats...");
        goto error;
      }

      if (msg->client_rtp_ports) {
        /* Get the RTP ports preferred by WFDSink */
        wfd_res =
            gst_wfd_message_get_prefered_rtp_ports (msg, &priv->ctrans,
            &priv->cprofile, &priv->clowertrans, &priv->crtp_port0,
            &priv->crtp_port1);
        if (wfd_res != GST_WFD_OK) {
          GST_WARNING_OBJECT (client,
              "Failed to get wfd prefered RTP ports...");
          goto error;
        }
      }

      if (msg->display_edid) {
        guint32 edid_block_count = 0;
        gchar *edid_payload = NULL;
        priv->edid_supported = FALSE;
        /* Get the display edid preferred by WFDSink */
        GST_DEBUG_OBJECT (client, "Going to gst_wfd_message_get_display_edid");
        wfd_res =
            gst_wfd_message_get_display_edid (msg, &priv->edid_supported,
            &edid_block_count, &edid_payload);
        if (wfd_res != GST_WFD_OK) {
          GST_ERROR_OBJECT (client, "Failed to get wfd display edid...");
          goto error;
        }
        GST_DEBUG_OBJECT (client, " edid supported: %d edid_block_count: %d",
            priv->edid_supported, edid_block_count);
        if (priv->edid_supported) {
          priv->edid_hres = 0;
          priv->edid_vres = 0;
          priv->edid_hres =
              (guint32) (((edid_payload[54 + 4] >> 4) << 8) | edid_payload[54 +
                  2]);
          priv->edid_vres =
              (guint32) (((edid_payload[54 + 7] >> 4) << 8) | edid_payload[54 +
                  5]);
          GST_DEBUG_OBJECT (client, " edid supported Hres: %d Wres: %d",
              priv->edid_hres, priv->edid_vres);
          if ((priv->edid_hres < 640) || (priv->edid_vres < 480)
              || (priv->edid_hres > 1920) || (priv->edid_vres > 1080)) {
            priv->edid_hres = 0;
            priv->edid_vres = 0;
            priv->edid_supported = FALSE;
            GST_WARNING_OBJECT (client, " edid invalid resolutions");
          }
        }
      }

      if (msg->content_protection) {
#if 0
        /*Get the hdcp version and tcp port by WFDSink */
        wfd_res =
            gst_wfd_message_get_contentprotection_type (msg,
            &priv->hdcp_version, &priv->hdcp_tcpport);
        GST_DEBUG ("hdcp version =%d, tcp port = %d", priv->hdcp_version,
            priv->hdcp_tcpport);
        if (priv->hdcp_version > 0 && priv->hdcp_tcpport > 0)
          priv->protection_enabled = TRUE;

        if (wfd_res != GST_WFD_OK) {
          GST_WARNING_OBJECT (client,
              "Failed to get wfd content protection...");
          goto error;
        }
#else
        GST_WARNING_OBJECT (client, "Don't use content protection");
#endif
      }

      g_signal_emit (_client,
          gst_rtsp_client_wfd_signals[SIGNAL_WFD_GET_PARAMETER_REQUEST], 0,
          ctx);
    } else {
      /* TODO-WFD: Handle another GET_PARAMETER response with body */
    }
  } else if (size == 0) {
    if (!priv->m1_done) {
      GST_INFO_OBJECT (_client, "M1 response is done");
      priv->m1_done = TRUE;
    } else if (!priv->m4_done) {
      GST_INFO_OBJECT (_client, "M4 response is done");
      priv->m4_done = TRUE;

      gst_rtsp_wfd_client_trigger_request (_client, WFD_TRIGGER_SETUP);
    }
  }

  return;

error:
  return;
}

static gboolean
handle_wfd_options_request (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPMethod options;
  gchar *tmp = NULL;
  gchar *str = NULL;
  gchar *user_agent = NULL;

  options = GST_RTSP_OPTIONS |
      GST_RTSP_PAUSE |
      GST_RTSP_PLAY |
      GST_RTSP_SETUP |
      GST_RTSP_GET_PARAMETER | GST_RTSP_SET_PARAMETER | GST_RTSP_TEARDOWN;

  str = gst_rtsp_options_as_text (options);

  /*append WFD specific method */
  tmp = g_strdup (", org.wfa.wfd1.0");
  g_strlcat (str, tmp, strlen (tmp) + strlen (str) + 1);

  gst_rtsp_message_init_response (ctx->response, GST_RTSP_STS_OK,
      gst_rtsp_status_as_text (GST_RTSP_STS_OK), ctx->request);

  gst_rtsp_message_add_header (ctx->response, GST_RTSP_HDR_PUBLIC, str);
  g_free (str);
  str = NULL;

  res =
      gst_rtsp_message_get_header (ctx->request, GST_RTSP_HDR_USER_AGENT,
      &user_agent, 0);
  if (res == GST_RTSP_OK) {
    gst_rtsp_message_add_header (ctx->response, GST_RTSP_HDR_USER_AGENT,
        user_agent);
  } else {
    return FALSE;
  }

  res = gst_rtsp_client_send_message (client, NULL, ctx->response);
  if (res != GST_RTSP_OK) {
    GST_ERROR_OBJECT (client, "gst_rtsp_client_send_message failed : %d", res);
    return FALSE;
  }

  GST_DEBUG_OBJECT (client, "Sent M2 response...");

  g_signal_emit (client,
      gst_rtsp_client_wfd_signals[SIGNAL_WFD_OPTIONS_REQUEST], 0, ctx);

  return TRUE;
}

static gboolean
handle_wfd_get_param_request (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPResult res = GST_RTSP_OK;
  guint8 *data = NULL;
  guint size = 0;

  GstRTSPWFDClient *_client = GST_RTSP_WFD_CLIENT (client);

  /* parsing the GET_PARAMTER request */
  res = gst_rtsp_message_get_body (ctx->request, (guint8 **) & data, &size);
  if (res != GST_RTSP_OK) {
    GST_ERROR_OBJECT (_client, "Failed to get body of request...");
    return FALSE;
  }

  if (size == 0) {
    send_generic_wfd_response (_client, GST_RTSP_STS_OK, ctx);
  } else {
    /* TODO-WFD: Handle other GET_PARAMETER request from sink */
  }

  return TRUE;
}

static gboolean
handle_wfd_set_param_request (GstRTSPClient * client, GstRTSPContext * ctx)
{
  return TRUE;
}

static gboolean
gst_rtsp_wfd_client_parse_methods (GstRTSPWFDClient * client,
    GstRTSPMessage * response)
{
  GstRTSPHeaderField field;
  gchar *respoptions;
  gchar **options;
  gint indx = 0;
  gint i;
  gboolean found_wfd_method = FALSE;

  /* reset supported methods */
  client->supported_methods = 0;

  /* Try Allow Header first */
  field = GST_RTSP_HDR_ALLOW;
  while (TRUE) {
    respoptions = NULL;
    gst_rtsp_message_get_header (response, field, &respoptions, indx);
    if (indx == 0 && !respoptions) {
      /* if no Allow header was found then try the Public header... */
      field = GST_RTSP_HDR_PUBLIC;
      gst_rtsp_message_get_header (response, field, &respoptions, indx);
    }
    if (!respoptions)
      break;

    /* If we get here, the server gave a list of supported methods, parse
     * them here. The string is like:
     *
     * OPTIONS,  PLAY, SETUP, ...
     */
    options = g_strsplit (respoptions, ",", 0);

    for (i = 0; options[i]; i++) {
      gchar *stripped;
      gint method;

      stripped = g_strstrip (options[i]);
      method = gst_rtsp_find_method (stripped);

      if (!g_ascii_strcasecmp ("org.wfa.wfd1.0", stripped))
        found_wfd_method = TRUE;

      /* keep bitfield of supported methods */
      if (method != GST_RTSP_INVALID)
        client->supported_methods |= method;
    }
    g_strfreev (options);

    indx++;
  }

  if (!found_wfd_method) {
    GST_ERROR_OBJECT (client,
        "WFD client is not supporting WFD mandatory message : org.wfa.wfd1.0...");
    goto no_required_methods;
  }

  /* Checking mandatory method */
  if (!(client->supported_methods & GST_RTSP_SET_PARAMETER)) {
    GST_ERROR_OBJECT (client,
        "WFD client is not supporting WFD mandatory message : SET_PARAMETER...");
    goto no_required_methods;
  }

  /* Checking mandatory method */
  if (!(client->supported_methods & GST_RTSP_GET_PARAMETER)) {
    GST_ERROR_OBJECT (client,
        "WFD client is not supporting WFD mandatory message : GET_PARAMETER...");
    goto no_required_methods;
  }

  if (!(client->supported_methods & GST_RTSP_OPTIONS)) {
    GST_INFO_OBJECT (client, "assuming OPTIONS is supported by client...");
    client->supported_methods |= GST_RTSP_OPTIONS;
  }

  return TRUE;

/* ERRORS */
no_required_methods:
  {
    GST_ELEMENT_ERROR (client, RESOURCE, OPEN_READ, (NULL),
        ("WFD Client does not support mandatory methods."));
    return FALSE;
  }
}

typedef enum
{
  M1_REQ_MSG,
  M1_RES_MSG,
  M2_REQ_MSG,
  M2_RES_MSG,
  M3_REQ_MSG,
  M3_RES_MSG,
  M4_REQ_MSG,
  M4_RES_MSG,
  M5_REQ_MSG,
  TEARDOWN_TRIGGER,
} GstWFDMessageType;

static void
_set_wfd_message_body (GstRTSPWFDClient * client, GstWFDMessageType msg_type,
    gchar ** data, guint * len)
{
  GString *buf = NULL;
  GstWFDMessage *msg = NULL;
  GstWFDResult wfd_res = GST_WFD_EINVAL;
  GstRTSPWFDClientPrivate *priv = GST_RTSP_WFD_CLIENT_GET_PRIVATE (client);

  buf = g_string_new ("");

  if (msg_type == M3_REQ_MSG) {
    /* create M3 request to be sent */
    wfd_res = gst_wfd_message_new (&msg);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client, "Failed to create wfd message...");
      goto error;
    }

    wfd_res = gst_wfd_message_init (msg);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client, "Failed to init wfd message...");
      goto error;
    }

    /* set the supported audio formats by the WFD server */
    wfd_res =
        gst_wfd_message_set_supported_audio_format (msg, GST_WFD_AUDIO_UNKNOWN,
        GST_WFD_FREQ_UNKNOWN, GST_WFD_CHANNEL_UNKNOWN, 0, 0);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client,
          "Failed to set supported audio formats on wfd message...");
      goto error;
    }

    /* set the supported Video formats by the WFD server */
    wfd_res =
        gst_wfd_message_set_supported_video_format (msg, GST_WFD_VIDEO_UNKNOWN,
        GST_WFD_VIDEO_CEA_RESOLUTION, GST_WFD_CEA_UNKNOWN, GST_WFD_CEA_UNKNOWN,
        GST_WFD_VESA_UNKNOWN, GST_WFD_HH_UNKNOWN, GST_WFD_H264_UNKNOWN_PROFILE,
        GST_WFD_H264_LEVEL_UNKNOWN, 0, 0, 0, 0, 0, 0);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client,
          "Failed to set supported video formats on wfd message...");
      goto error;
    }

    wfd_res = gst_wfd_message_set_display_edid (msg, 0, 0, NULL);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client,
          "Failed to set display edid type on wfd message...");
      goto error;
    }

    if (priv->protection_enabled) {
      wfd_res =
          gst_wfd_message_set_contentprotection_type (msg, GST_WFD_HDCP_NONE,
          0);
      if (wfd_res != GST_WFD_OK) {
        GST_ERROR_OBJECT (client,
            "Failed to set supported content protection type on wfd message...");
        goto error;
      }
    }

    /* set the preffered RTP ports for the WFD server */
    wfd_res =
        gst_wfd_messge_set_prefered_rtp_ports (msg, GST_WFD_RTSP_TRANS_UNKNOWN,
        GST_WFD_RTSP_PROFILE_UNKNOWN, GST_WFD_RTSP_LOWER_TRANS_UNKNOWN, 0, 0);
    if (wfd_res != GST_WFD_OK) {
      GST_ERROR_OBJECT (client,
          "Failed to set supported video formats on wfd message...");
      goto error;
    }

    *data = gst_wfd_message_param_names_as_text (msg);
    if (*data == NULL) {
      GST_ERROR_OBJECT (client, "Failed to get wfd message as text...");
      goto error;
    } else {
      *len = strlen (*data);
    }
  } else if (msg_type == M4_REQ_MSG) {
    GstRTSPUrl *url = NULL;

    GstRTSPClient *parent_client = GST_RTSP_CLIENT_CAST (client);
    GstRTSPConnection *connection =
        gst_rtsp_client_get_connection (parent_client);

    url = gst_rtsp_connection_get_url (connection);
    if (url == NULL) {
      GST_ERROR_OBJECT (client, "Failed to get connection URL");
      return;
    }

    /* TODO-WFD
       1. Define WFD message as a structure just like GstSDPMessage
       2. Logic to negotiate with M3 response
     */
    g_string_append (buf, "wfd_audio_codecs: AAC 00000001 00");
    g_string_append (buf, "\r\n");
    //g_string_append(buf, "wfd_video_formats: 00 00 01 01 000000ab 00000000 00000000 00 0000 0000 00 02d0 0500");
    //g_string_append(buf, "wfd_video_formats: 00 00 01 01 0000002b 00000000 00000000 00 0000 0000 00 02d0 0500");
    g_string_append (buf,
        "wfd_video_formats: 00 00 01 01 00000001 00000000 00000000 00 0000 0000 00 02d0 0500");
    g_string_append (buf, "\r\n");
    g_string_append (buf, "wfd_presentation_URL: ");
    g_string_append_printf (buf, "rtsp://%s/wfd/streamid=0 none", url->host);
    g_string_append (buf, "\r\n");
    g_string_append (buf, "wfd_content_protection: none");
    g_string_append (buf, "\r\n");
    g_string_append (buf,
        "wfd_client_rtp_ports: RTP/AVP/UDP;unicast 19000 0 mode=play");
    g_string_append (buf, "\r\n");
    *len = buf->len;
    *data = g_string_free (buf, FALSE);
  } else if (msg_type == M5_REQ_MSG) {
    g_string_append (buf, "wfd_trigger_method: SETUP");
    g_string_append (buf, "\r\n");
    *len = buf->len;
    *data = g_string_free (buf, FALSE);
  } else if (msg_type == TEARDOWN_TRIGGER) {
    g_string_append (buf, "wfd_trigger_method: TEARDOWN");
    g_string_append (buf, "\r\n");
    *len = buf->len;
    *data = g_string_free (buf, FALSE);
  } else {
    return;
  }

  return;

error:
  *data = NULL;
  *len = 0;

  return;
}

/**
* prepare_request:
* @client: client object
* @request : requst message to be prepared
* @method : RTSP method of the request
* @url : url need to be in the request
* @message_type : WFD message type
* @trigger_type : trigger method to be used for M5 mainly
*
* Prepares request based on @method & @message_type
*
* Returns: a #GstRTSPResult.
*/
GstRTSPResult
prepare_request (GstRTSPWFDClient * client, GstRTSPMessage * request,
    GstRTSPMethod method, gchar * url)
{
  GstRTSPResult res = GST_RTSP_OK;
  gchar *str = NULL;

  if (method == GST_RTSP_GET_PARAMETER || method == GST_RTSP_SET_PARAMETER) {
    g_free (url);
    url = g_strdup ("rtsp://localhost/wfd1.0");
  }

  GST_DEBUG_OBJECT (client, "Preparing request: %d", method);

  /* initialize the request */
  res = gst_rtsp_message_init_request (request, method, url);
  if (res < 0) {
    GST_ERROR ("init request failed");
    return res;
  }

  switch (method) {
      /* Prepare OPTIONS request to send */
    case GST_RTSP_OPTIONS:{
      /* add wfd specific require filed "org.wfa.wfd1.0" */
      str = g_strdup ("org.wfa.wfd1.0");
      res = gst_rtsp_message_add_header (request, GST_RTSP_HDR_REQUIRE, str);
      if (res < 0) {
        GST_ERROR ("Failed to add header");
        g_free (str);
        return res;
      }

      g_free (str);
      break;
    }

      /* Prepare GET_PARAMETER request */
    case GST_RTSP_GET_PARAMETER:{
      gchar *msg = NULL;
      guint msglen = 0;
      GString *msglength;

      /* add content type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_TYPE,
          "text/parameters");
      if (res < 0) {
        GST_ERROR ("Failed to add header");
        return res;
      }

      _set_wfd_message_body (client, M3_REQ_MSG, &msg, &msglen);
      msglength = g_string_new ("");
      g_string_append_printf (msglength, "%d", msglen);
      GST_DEBUG ("M3 server side message body: %s", msg);

      /* add content-length type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_LENGTH,
          g_string_free (msglength, FALSE));
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      res = gst_rtsp_message_set_body (request, (guint8 *) msg, msglen);
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      g_free (msg);
      break;
    }

      /* Prepare SET_PARAMETER request */
    case GST_RTSP_SET_PARAMETER:{
      gchar *msg;
      guint msglen = 0;
      GString *msglength;

      /* add content type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_TYPE,
          "text/parameters");
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp request...");
        goto error;
      }

      _set_wfd_message_body (client, M4_REQ_MSG, &msg, &msglen);
      msglength = g_string_new ("");
      g_string_append_printf (msglength, "%d", msglen);
      GST_DEBUG ("M4 server side message body: %s", msg);

      /* add content-length type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_LENGTH,
          g_string_free (msglength, FALSE));
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      res = gst_rtsp_message_set_body (request, (guint8 *) msg, msglen);
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      g_free (msg);
      break;
    }

    default:{
    }
  }

  return res;

error:
  return GST_RTSP_ERROR;
}

GstRTSPResult
prepare_trigger_request (GstRTSPWFDClient * client, GstRTSPMessage * request,
    GstWFDTriggerType trigger_type, gchar * url)
{
  GstRTSPResult res = GST_RTSP_OK;

  /* initialize the request */
  res = gst_rtsp_message_init_request (request, GST_RTSP_SET_PARAMETER, url);
  if (res < 0) {
    GST_ERROR ("init request failed");
    return res;
  }

  switch (trigger_type) {
    case WFD_TRIGGER_SETUP:{
      gchar *msg;
      guint msglen = 0;
      GString *msglength;

      /* add content type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_TYPE,
          "text/parameters");
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp request...");
        goto error;
      }

      _set_wfd_message_body (client, M5_REQ_MSG, &msg, &msglen);
      msglength = g_string_new ("");
      g_string_append_printf (msglength, "%d", msglen);
      GST_DEBUG ("M5 server side message body: %s", msg);

      /* add content-length type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_LENGTH,
          g_string_free (msglength, FALSE));
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      res = gst_rtsp_message_set_body (request, (guint8 *) msg, msglen);
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      g_free (msg);
      break;
    }
    case WFD_TRIGGER_TEARDOWN:{
      gchar *msg;
      guint msglen = 0;
      GString *msglength;

      /* add content type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_TYPE,
          "text/parameters");
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp request...");
        goto error;
      }

      _set_wfd_message_body (client, TEARDOWN_TRIGGER, &msg, &msglen);
      msglength = g_string_new ("");
      g_string_append_printf (msglength, "%d", msglen);
      GST_DEBUG ("Trigger TEARDOWN server side message body: %s", msg);

      /* add content-length type */
      res =
          gst_rtsp_message_add_header (request, GST_RTSP_HDR_CONTENT_LENGTH,
          g_string_free (msglength, FALSE));
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      res = gst_rtsp_message_set_body (request, (guint8 *) msg, msglen);
      if (res != GST_RTSP_OK) {
        GST_ERROR_OBJECT (client, "Failed to add header to rtsp message...");
        goto error;
      }

      g_free (msg);
      break;
    }
      /* TODO-WFD: implement to handle other trigger type */
    default:{
    }
  }

  return res;

error:
  return res;
}


void
send_request (GstRTSPWFDClient * client, GstRTSPSession * session,
    GstRTSPMessage * request)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPClient *parent_client = GST_RTSP_CLIENT_CAST (client);

  /* remove any previous header */
  gst_rtsp_message_remove_header (request, GST_RTSP_HDR_SESSION, -1);

  /* add the new session header for new session ids */
  if (session) {
    guint timeout;
    const gchar *sessionid = NULL;
    gchar *str;

    sessionid = gst_rtsp_session_get_sessionid (session);
    GST_INFO_OBJECT (client, "Session id : %s", sessionid);

    timeout = gst_rtsp_session_get_timeout (session);
    if (timeout != DEFAULT_WFD_TIMEOUT)
      str = g_strdup_printf ("%s; timeout=%d", sessionid, timeout);
    else
      str = g_strdup (sessionid);

    gst_rtsp_message_take_header (request, GST_RTSP_HDR_SESSION, str);
  }
#if 0
  if (gst_debug_category_get_threshold (rtsp_wfd_client_debug) >= GST_LEVEL_LOG) {
    gst_rtsp_message_dump (request);
  }
#endif
  res = gst_rtsp_client_send_message (parent_client, session, request);
  if (res != GST_RTSP_OK) {
    GST_ERROR_OBJECT (client, "gst_rtsp_client_send_message failed : %d", res);
  }

  gst_rtsp_message_unset (request);
}

/**
* prepare_response:
* @client: client object
* @request : requst message received
* @response : response to be prepare based on request
* @method : RTSP method
*
* prepare response to the request based on @method & @message_type
*
* Returns: a #GstRTSPResult.
*/
GstRTSPResult
prepare_response (GstRTSPWFDClient * client, GstRTSPMessage * request,
    GstRTSPMessage * response, GstRTSPMethod method)
{
  GstRTSPResult res = GST_RTSP_OK;

  switch (method) {
      /* prepare OPTIONS response */
    case GST_RTSP_OPTIONS:{
      GstRTSPMethod options;
      gchar *tmp = NULL;
      gchar *str = NULL;
      gchar *user_agent = NULL;

      options = GST_RTSP_OPTIONS |
          GST_RTSP_PAUSE |
          GST_RTSP_PLAY |
          GST_RTSP_SETUP |
          GST_RTSP_GET_PARAMETER | GST_RTSP_SET_PARAMETER | GST_RTSP_TEARDOWN;

      str = gst_rtsp_options_as_text (options);

      /*append WFD specific method */
      tmp = g_strdup (", org.wfa.wfd1.0");
      g_strlcat (str, tmp, strlen (tmp) + strlen (str) + 1);

      gst_rtsp_message_init_response (response, GST_RTSP_STS_OK,
          gst_rtsp_status_as_text (GST_RTSP_STS_OK), request);

      gst_rtsp_message_add_header (response, GST_RTSP_HDR_PUBLIC, str);
      g_free (str);
      str = NULL;
      res =
          gst_rtsp_message_get_header (request, GST_RTSP_HDR_USER_AGENT,
          &user_agent, 0);
      if (res == GST_RTSP_OK) {
        gst_rtsp_message_add_header (response, GST_RTSP_HDR_USER_AGENT,
            user_agent);
      } else
        res = GST_RTSP_OK;
      break;
    }
    default:
      GST_ERROR_OBJECT (client, "Unhandled method...");
      return GST_RTSP_EINVAL;
      break;
  }

  return res;
}

static void
send_generic_wfd_response (GstRTSPWFDClient * client, GstRTSPStatusCode code,
    GstRTSPContext * ctx)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPClient *parent_client = GST_RTSP_CLIENT_CAST (client);

  gst_rtsp_message_init_response (ctx->response, code,
      gst_rtsp_status_as_text (code), ctx->request);

  res = gst_rtsp_client_send_message (parent_client, NULL, ctx->response);
  if (res != GST_RTSP_OK) {
    GST_ERROR_OBJECT (client, "gst_rtsp_client_send_message failed : %d", res);
  }
}


static GstRTSPResult
handle_M1_message (GstRTSPWFDClient * client)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPMessage request = { 0 };

  res = prepare_request (client, &request, GST_RTSP_OPTIONS, (gchar *) "*");
  if (GST_RTSP_OK != res) {
    GST_ERROR_OBJECT (client, "Failed to prepare M1 request....\n");
    return res;
  }

  GST_DEBUG_OBJECT (client, "Sending M1 request.. (OPTIONS request)");

  send_request (client, NULL, &request);

  return res;
}

/**
* handle_M3_message:
* @client: client object
*
* Handles M3 WFD message.
* This API will send M3 message (GET_PARAMETER) to WFDSink to query supported formats by the WFDSink.
* After getting supported formats info, this API will set those values on WFDConfigMessage obj
*
* Returns: a #GstRTSPResult.
*/
static GstRTSPResult
handle_M3_message (GstRTSPWFDClient * client)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPMessage request = { 0 };
  GstRTSPUrl *url = NULL;
  gchar *url_str = NULL;

  GstRTSPClient *parent_client = GST_RTSP_CLIENT_CAST (client);
  GstRTSPConnection *connection =
      gst_rtsp_client_get_connection (parent_client);

  url = gst_rtsp_connection_get_url (connection);
  if (url == NULL) {
    GST_ERROR_OBJECT (client, "Failed to get connection URL");
    res = GST_RTSP_ERROR;
    goto error;
  }

  url_str = gst_rtsp_url_get_request_uri (url);
  if (url_str == NULL) {
    GST_ERROR_OBJECT (client, "Failed to get connection URL");
    res = GST_RTSP_ERROR;
    goto error;
  }

  res = prepare_request (client, &request, GST_RTSP_GET_PARAMETER, url_str);
  if (GST_RTSP_OK != res) {
    GST_ERROR_OBJECT (client, "Failed to prepare M3 request....\n");
    goto error;
  }

  GST_DEBUG_OBJECT (client, "Sending GET_PARAMETER request message (M3)...");

  send_request (client, NULL, &request);

  return res;

error:
  return res;
}

static GstRTSPResult
handle_M4_message (GstRTSPWFDClient * client)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPMessage request = { 0 };
  GstRTSPUrl *url = NULL;
  gchar *url_str = NULL;

  GstRTSPClient *parent_client = GST_RTSP_CLIENT_CAST (client);
  GstRTSPConnection *connection =
      gst_rtsp_client_get_connection (parent_client);

  url = gst_rtsp_connection_get_url (connection);
  if (url == NULL) {
    GST_ERROR_OBJECT (client, "Failed to get connection URL");
    res = GST_RTSP_ERROR;
    goto error;
  }

  url_str = gst_rtsp_url_get_request_uri (url);
  if (url_str == NULL) {
    GST_ERROR_OBJECT (client, "Failed to get connection URL");
    res = GST_RTSP_ERROR;
    goto error;
  }

  res = prepare_request (client, &request, GST_RTSP_SET_PARAMETER, url_str);
  if (GST_RTSP_OK != res) {
    GST_ERROR_OBJECT (client, "Failed to prepare M3 request....\n");
    goto error;
  }

  GST_DEBUG_OBJECT (client, "Sending GET_PARAMETER request message (M3)...");

  send_request (client, NULL, &request);

  return res;

error:
  return res;
}

GstRTSPResult
gst_rtsp_wfd_client_trigger_request (GstRTSPWFDClient * client,
    GstWFDTriggerType type)
{
  GstRTSPResult res = GST_RTSP_OK;
  GstRTSPMessage request = { 0 };
  GstRTSPUrl *url = NULL;
  gchar *url_str = NULL;

  GstRTSPClient *parent_client = GST_RTSP_CLIENT_CAST (client);
  GstRTSPConnection *connection =
      gst_rtsp_client_get_connection (parent_client);

  url = gst_rtsp_connection_get_url (connection);
  if (url == NULL) {
    GST_ERROR_OBJECT (client, "Failed to get connection URL");
    res = GST_RTSP_ERROR;
    goto error;
  }

  url_str = gst_rtsp_url_get_request_uri (url);
  if (url_str == NULL) {
    GST_ERROR_OBJECT (client, "Failed to get connection URL");
    res = GST_RTSP_ERROR;
    goto error;
  }

  res = prepare_trigger_request (client, &request, type, url_str);
  if (GST_RTSP_OK != res) {
    GST_ERROR_OBJECT (client, "Failed to prepare M5 request....\n");
    goto error;
  }

  GST_DEBUG_OBJECT (client, "Sending trigger request message...: %d", type);

  send_request (client, NULL, &request);

  return res;

error:
  return res;
}
