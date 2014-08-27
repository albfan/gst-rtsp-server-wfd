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
 * A #GstRTSPClient is created by #GstRTSPServer when a new connection is
 * accepted and it inherits the #GstRTSPMountPoints, #GstRTSPSessionPool,
 * #GstRTSPAuth and #GstRTSPThreadPool from the server.
 *
 * The client connection should be configured with the #GstRTSPConnection using
 * gst_rtsp_client_set_connection() before it can be attached to a #GMainContext
 * using gst_rtsp_client_attach(). From then on the client will handle requests
 * on the connection.
 *
 * Use gst_rtsp_client_session_filter() to iterate or modify all the
 * #GstRTSPSession objects managed by the client object.
 *
 * Last reviewed on 2013-07-11 (1.0.0)
 */

#include <stdio.h>
#include <string.h>

#include "rtsp-client.h"
#include "rtsp-sdp.h"
#include "rtsp-params.h"

#define GST_RTSP_CLIENT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_RTSP_CLIENT, GstRTSPClientPrivate))

/* locking order:
 * send_lock, lock, tunnels_lock
 */

struct _GstRTSPClientPrivate
{
  GMutex lock;                  /* protects everything else */
  GMutex send_lock;
  GstRTSPConnection *connection;
  GstRTSPWatch *watch;
  guint close_seq;
  gchar *server_ip;
  gboolean is_ipv6;

  GstRTSPClientSendFunc send_func;      /* protected by send_lock */
  gpointer send_data;           /* protected by send_lock */
  GDestroyNotify send_notify;   /* protected by send_lock */

  GstRTSPSessionPool *session_pool;
  GstRTSPMountPoints *mount_points;
  GstRTSPAuth *auth;
  GstRTSPThreadPool *thread_pool;

  /* used to cache the media in the last requested DESCRIBE so that
   * we can pick it up in the next SETUP immediately */
  gchar *path;
  GstRTSPMedia *media;

  GList *transports;
  GList *sessions;
};

static GMutex tunnels_lock;
static GHashTable *tunnels;     /* protected by tunnels_lock */

#define DEFAULT_SESSION_POOL            NULL
#define DEFAULT_MOUNT_POINTS            NULL

enum
{
  PROP_0,
  PROP_SESSION_POOL,
  PROP_MOUNT_POINTS,
  PROP_LAST
};

enum
{
  SIGNAL_CLOSED,
  SIGNAL_NEW_SESSION,
  SIGNAL_OPTIONS_REQUEST,
  SIGNAL_DESCRIBE_REQUEST,
  SIGNAL_SETUP_REQUEST,
  SIGNAL_PLAY_REQUEST,
  SIGNAL_PAUSE_REQUEST,
  SIGNAL_TEARDOWN_REQUEST,
  SIGNAL_SET_PARAMETER_REQUEST,
  SIGNAL_GET_PARAMETER_REQUEST,
  SIGNAL_HANDLE_RESPONSE,
  SIGNAL_LAST
};

GST_DEBUG_CATEGORY_STATIC (rtsp_client_debug);
#define GST_CAT_DEFAULT rtsp_client_debug

static guint gst_rtsp_client_signals[SIGNAL_LAST] = { 0 };

static void gst_rtsp_client_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec);
static void gst_rtsp_client_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec);
static void gst_rtsp_client_finalize (GObject * obj);

static GstSDPMessage *create_sdp (GstRTSPClient * client, GstRTSPMedia * media);
static void client_session_finalized (GstRTSPClient * client,
    GstRTSPSession * session);
static void unlink_session_transports (GstRTSPClient * client,
    GstRTSPSession * session, GstRTSPSessionMedia * sessmedia);
static gboolean default_configure_client_media (GstRTSPClient * client,
    GstRTSPMedia * media, GstRTSPStream * stream, GstRTSPContext * ctx);
static gboolean default_configure_client_transport (GstRTSPClient * client,
    GstRTSPContext * ctx, GstRTSPTransport * ct);
static GstRTSPResult default_params_set (GstRTSPClient * client,
    GstRTSPContext * ctx);
static GstRTSPResult default_params_get (GstRTSPClient * client,
    GstRTSPContext * ctx);
static gchar *default_make_path_from_uri (GstRTSPClient * client,
    const GstRTSPUrl * uri);
static gboolean default_handle_options_request (GstRTSPClient * client,
    GstRTSPContext * ctx);
static gboolean default_handle_set_param_request (GstRTSPClient * client,
    GstRTSPContext * ctx);
static gboolean default_handle_get_param_request (GstRTSPClient * client,
    GstRTSPContext * ctx);

G_DEFINE_TYPE (GstRTSPClient, gst_rtsp_client, G_TYPE_OBJECT);

static void
gst_rtsp_client_class_init (GstRTSPClientClass * klass)
{
  GObjectClass *gobject_class;

  g_type_class_add_private (klass, sizeof (GstRTSPClientPrivate));

  gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = gst_rtsp_client_get_property;
  gobject_class->set_property = gst_rtsp_client_set_property;
  gobject_class->finalize = gst_rtsp_client_finalize;

  klass->create_sdp = create_sdp;
  klass->configure_client_media = default_configure_client_media;
  klass->configure_client_transport = default_configure_client_transport;
  klass->params_set = default_params_set;
  klass->params_get = default_params_get;
  klass->make_path_from_uri = default_make_path_from_uri;
  klass->handle_options_request = default_handle_options_request;
  klass->handle_set_param_request = default_handle_set_param_request;
  klass->handle_get_param_request = default_handle_get_param_request;

  g_object_class_install_property (gobject_class, PROP_SESSION_POOL,
      g_param_spec_object ("session-pool", "Session Pool",
          "The session pool to use for client session",
          GST_TYPE_RTSP_SESSION_POOL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MOUNT_POINTS,
      g_param_spec_object ("mount-points", "Mount Points",
          "The mount points to use for client session",
          GST_TYPE_RTSP_MOUNT_POINTS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_rtsp_client_signals[SIGNAL_CLOSED] =
      g_signal_new ("closed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstRTSPClientClass, closed), NULL, NULL,
      g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0, G_TYPE_NONE);

  gst_rtsp_client_signals[SIGNAL_NEW_SESSION] =
      g_signal_new ("new-session", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstRTSPClientClass, new_session), NULL, NULL,
      g_cclosure_marshal_VOID__OBJECT, G_TYPE_NONE, 1, GST_TYPE_RTSP_SESSION);

  gst_rtsp_client_signals[SIGNAL_OPTIONS_REQUEST] =
      g_signal_new ("options-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass, options_request),
      NULL, NULL, g_cclosure_marshal_VOID__POINTER, G_TYPE_NONE, 1,
      G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_DESCRIBE_REQUEST] =
      g_signal_new ("describe-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass, describe_request),
      NULL, NULL, g_cclosure_marshal_VOID__POINTER, G_TYPE_NONE, 1,
      G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_SETUP_REQUEST] =
      g_signal_new ("setup-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass, setup_request),
      NULL, NULL, g_cclosure_marshal_VOID__POINTER, G_TYPE_NONE, 1,
      G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_PLAY_REQUEST] =
      g_signal_new ("play-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass, play_request),
      NULL, NULL, g_cclosure_marshal_VOID__POINTER, G_TYPE_NONE, 1,
      G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_PAUSE_REQUEST] =
      g_signal_new ("pause-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass, pause_request),
      NULL, NULL, g_cclosure_marshal_VOID__POINTER, G_TYPE_NONE, 1,
      G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_TEARDOWN_REQUEST] =
      g_signal_new ("teardown-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass, teardown_request),
      NULL, NULL, g_cclosure_marshal_VOID__POINTER, G_TYPE_NONE, 1,
      G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_SET_PARAMETER_REQUEST] =
      g_signal_new ("set-parameter-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass,
          set_parameter_request), NULL, NULL, g_cclosure_marshal_VOID__POINTER,
      G_TYPE_NONE, 1, G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_GET_PARAMETER_REQUEST] =
      g_signal_new ("get-parameter-request", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass,
          get_parameter_request), NULL, NULL, g_cclosure_marshal_VOID__POINTER,
      G_TYPE_NONE, 1, G_TYPE_POINTER);

  gst_rtsp_client_signals[SIGNAL_HANDLE_RESPONSE] =
      g_signal_new ("handle-response", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstRTSPClientClass,
          handle_response), NULL, NULL, g_cclosure_marshal_VOID__POINTER,
      G_TYPE_NONE, 1, G_TYPE_POINTER);

  tunnels =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  g_mutex_init (&tunnels_lock);

  GST_DEBUG_CATEGORY_INIT (rtsp_client_debug, "rtspclient", 0, "GstRTSPClient");
}

static void
gst_rtsp_client_init (GstRTSPClient * client)
{
  GstRTSPClientPrivate *priv = GST_RTSP_CLIENT_GET_PRIVATE (client);

  client->priv = priv;

  g_mutex_init (&priv->lock);
  g_mutex_init (&priv->send_lock);
  priv->close_seq = 0;
}

static GstRTSPFilterResult
filter_session (GstRTSPSession * sess, GstRTSPSessionMedia * sessmedia,
    gpointer user_data)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (user_data);

  gst_rtsp_session_media_set_state (sessmedia, GST_STATE_NULL);
  unlink_session_transports (client, sess, sessmedia);

  /* unmanage the media in the session */
  return GST_RTSP_FILTER_REMOVE;
}

static void
client_unlink_session (GstRTSPClient * client, GstRTSPSession * session)
{
  /* unlink all media managed in this session */
  gst_rtsp_session_filter (session, filter_session, client);
}

static void
client_watch_session (GstRTSPClient * client, GstRTSPSession * session)
{
  GstRTSPClientPrivate *priv = client->priv;
  GList *walk;

  for (walk = priv->sessions; walk; walk = g_list_next (walk)) {
    GstRTSPSession *msession = (GstRTSPSession *) walk->data;

    /* we already know about this session */
    if (msession == session)
      return;
  }

  GST_INFO ("watching session %p", session);

  g_object_weak_ref (G_OBJECT (session), (GWeakNotify) client_session_finalized,
      client);
  priv->sessions = g_list_prepend (priv->sessions, session);
}

static void
client_unwatch_session (GstRTSPClient * client, GstRTSPSession * session)
{
  GstRTSPClientPrivate *priv = client->priv;

  GST_INFO ("unwatching session %p", session);

  g_object_weak_unref (G_OBJECT (session),
      (GWeakNotify) client_session_finalized, client);
  priv->sessions = g_list_remove (priv->sessions, session);
}

static void
client_cleanup_session (GstRTSPClient * client, GstRTSPSession * session)
{
  g_object_weak_unref (G_OBJECT (session),
      (GWeakNotify) client_session_finalized, client);
  client_unlink_session (client, session);
}

static void
client_cleanup_sessions (GstRTSPClient * client)
{
  GstRTSPClientPrivate *priv = client->priv;
  GList *sessions;

  /* remove weak-ref from sessions */
  for (sessions = priv->sessions; sessions; sessions = g_list_next (sessions)) {
    client_cleanup_session (client, (GstRTSPSession *) sessions->data);
  }
  g_list_free (priv->sessions);
  priv->sessions = NULL;
}

/* A client is finalized when the connection is broken */
static void
gst_rtsp_client_finalize (GObject * obj)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (obj);
  GstRTSPClientPrivate *priv = client->priv;

  GST_INFO ("finalize client %p", client);

  gst_rtsp_client_set_send_func (client, NULL, NULL, NULL);

  if (priv->watch)
    g_source_destroy ((GSource *) priv->watch);

  client_cleanup_sessions (client);

  if (priv->connection)
    gst_rtsp_connection_free (priv->connection);
  if (priv->session_pool)
    g_object_unref (priv->session_pool);
  if (priv->mount_points)
    g_object_unref (priv->mount_points);
  if (priv->auth)
    g_object_unref (priv->auth);
  if (priv->thread_pool)
    g_object_unref (priv->thread_pool);

  if (priv->path)
    g_free (priv->path);
  if (priv->media) {
    gst_rtsp_media_unprepare (priv->media);
    g_object_unref (priv->media);
  }

  g_free (priv->server_ip);
  g_mutex_clear (&priv->lock);
  g_mutex_clear (&priv->send_lock);

  G_OBJECT_CLASS (gst_rtsp_client_parent_class)->finalize (obj);
}

static void
gst_rtsp_client_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (object);

  switch (propid) {
    case PROP_SESSION_POOL:
      g_value_take_object (value, gst_rtsp_client_get_session_pool (client));
      break;
    case PROP_MOUNT_POINTS:
      g_value_take_object (value, gst_rtsp_client_get_mount_points (client));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

static void
gst_rtsp_client_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (object);

  switch (propid) {
    case PROP_SESSION_POOL:
      gst_rtsp_client_set_session_pool (client, g_value_get_object (value));
      break;
    case PROP_MOUNT_POINTS:
      gst_rtsp_client_set_mount_points (client, g_value_get_object (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

/**
 * gst_rtsp_client_new:
 *
 * Create a new #GstRTSPClient instance.
 *
 * Returns: a new #GstRTSPClient
 */
GstRTSPClient *
gst_rtsp_client_new (void)
{
  GstRTSPClient *result;

  result = g_object_new (GST_TYPE_RTSP_CLIENT, NULL);

  return result;
}

static void
send_message (GstRTSPClient * client, GstRTSPSession * session,
    GstRTSPMessage * message, gboolean close)
{
  GstRTSPClientPrivate *priv = client->priv;

  gst_rtsp_message_add_header (message, GST_RTSP_HDR_SERVER,
      "GStreamer RTSP server");

  /* remove any previous header */
  gst_rtsp_message_remove_header (message, GST_RTSP_HDR_SESSION, -1);

  /* add the new session header for new session ids */
  if (session) {
    gst_rtsp_message_take_header (message, GST_RTSP_HDR_SESSION,
        gst_rtsp_session_get_header (session));
  }

  if (gst_debug_category_get_threshold (rtsp_client_debug) >= GST_LEVEL_LOG) {
    gst_rtsp_message_dump (message);
  }

  if (close)
    gst_rtsp_message_add_header (message, GST_RTSP_HDR_CONNECTION, "close");

  g_mutex_lock (&priv->send_lock);
  if (priv->send_func)
    priv->send_func (client, message, close, priv->send_data);
  g_mutex_unlock (&priv->send_lock);

  gst_rtsp_message_unset (message);
}

static void
send_generic_response (GstRTSPClient * client, GstRTSPStatusCode code,
    GstRTSPContext * ctx)
{
  gst_rtsp_message_init_response (ctx->response, code,
      gst_rtsp_status_as_text (code), ctx->request);

  send_message (client, NULL, ctx->response, FALSE);
}

static gboolean
paths_are_equal (const gchar * path1, const gchar * path2, gint len2)
{
  if (path1 == NULL || path2 == NULL)
    return FALSE;

  if (strlen (path1) != len2)
    return FALSE;

  if (strncmp (path1, path2, len2))
    return FALSE;

  return TRUE;
}

/* this function is called to initially find the media for the DESCRIBE request
 * but is cached for when the same client (without breaking the connection) is
 * doing a setup for the exact same url. */
static GstRTSPMedia *
find_media (GstRTSPClient * client, GstRTSPContext * ctx, gchar * path,
    gint * matched)
{
  GstRTSPClientPrivate *priv = client->priv;
  GstRTSPMediaFactory *factory;
  GstRTSPMedia *media;
  gint path_len;

  /* find the longest matching factory for the uri first */
  if (!(factory = gst_rtsp_mount_points_match (priv->mount_points,
              path, matched)))
    goto no_factory;

  ctx->factory = factory;

  if (!gst_rtsp_auth_check (GST_RTSP_AUTH_CHECK_MEDIA_FACTORY_ACCESS))
    goto no_factory_access;

  if (!gst_rtsp_auth_check (GST_RTSP_AUTH_CHECK_MEDIA_FACTORY_CONSTRUCT))
    goto not_authorized;

  if (matched)
    path_len = *matched;
  else
    path_len = strlen (path);

  if (!paths_are_equal (priv->path, path, path_len)) {
    GstRTSPThread *thread;

    /* remove any previously cached values before we try to construct a new
     * media for uri */
    if (priv->path)
      g_free (priv->path);
    priv->path = NULL;
    if (priv->media) {
      gst_rtsp_media_unprepare (priv->media);
      g_object_unref (priv->media);
    }
    priv->media = NULL;

    /* prepare the media and add it to the pipeline */
    if (!(media = gst_rtsp_media_factory_construct (factory, ctx->uri)))
      goto no_media;

    ctx->media = media;

    thread = gst_rtsp_thread_pool_get_thread (priv->thread_pool,
        GST_RTSP_THREAD_TYPE_MEDIA, ctx);
    if (thread == NULL)
      goto no_thread;

    /* prepare the media */
    if (!(gst_rtsp_media_prepare (media, thread)))
      goto no_prepare;

    /* now keep track of the uri and the media */
    priv->path = g_strndup (path, path_len);
    priv->media = media;
  } else {
    /* we have seen this path before, used cached media */
    media = priv->media;
    ctx->media = media;
    GST_INFO ("reusing cached media %p for path %s", media, priv->path);
  }

  g_object_unref (factory);
  ctx->factory = NULL;

  if (media)
    g_object_ref (media);

  return media;

  /* ERRORS */
no_factory:
  {
    GST_ERROR ("client %p: no factory for path %s", client, path);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, ctx);
    return NULL;
  }
no_factory_access:
  {
    GST_ERROR ("client %p: not authorized to see factory path %s", client,
        path);
    /* error reply is already sent */
    return NULL;
  }
not_authorized:
  {
    GST_ERROR ("client %p: not authorized for factory path %s", client, path);
    /* error reply is already sent */
    return NULL;
  }
no_media:
  {
    GST_ERROR ("client %p: can't create media", client);
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, ctx);
    g_object_unref (factory);
    ctx->factory = NULL;
    return NULL;
  }
no_thread:
  {
    GST_ERROR ("client %p: can't create thread", client);
    send_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, ctx);
    g_object_unref (media);
    ctx->media = NULL;
    g_object_unref (factory);
    ctx->factory = NULL;
    return NULL;
  }
no_prepare:
  {
    GST_ERROR ("client %p: can't prepare media", client);
    send_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, ctx);
    g_object_unref (media);
    ctx->media = NULL;
    g_object_unref (factory);
    ctx->factory = NULL;
    return NULL;
  }
}

static gboolean
do_send_data (GstBuffer * buffer, guint8 channel, GstRTSPClient * client)
{
  GstRTSPClientPrivate *priv = client->priv;
  GstRTSPMessage message = { 0 };
  GstMapInfo map_info;
  guint8 *data;
  guint usize;

  gst_rtsp_message_init_data (&message, channel);

  /* FIXME, need some sort of iovec RTSPMessage here */
  if (!gst_buffer_map (buffer, &map_info, GST_MAP_READ))
    return FALSE;

  gst_rtsp_message_take_body (&message, map_info.data, map_info.size);

  g_mutex_lock (&priv->send_lock);
  if (priv->send_func)
    priv->send_func (client, &message, FALSE, priv->send_data);
  g_mutex_unlock (&priv->send_lock);

  gst_rtsp_message_steal_body (&message, &data, &usize);
  gst_buffer_unmap (buffer, &map_info);

  gst_rtsp_message_unset (&message);

  return TRUE;
}

static void
link_transport (GstRTSPClient * client, GstRTSPSession * session,
    GstRTSPStreamTransport * trans)
{
  GstRTSPClientPrivate *priv = client->priv;

  GST_DEBUG ("client %p: linking transport %p", client, trans);

  gst_rtsp_stream_transport_set_callbacks (trans,
      (GstRTSPSendFunc) do_send_data,
      (GstRTSPSendFunc) do_send_data, client, NULL);

  priv->transports = g_list_prepend (priv->transports, trans);

  /* make sure our session can't expire */
  gst_rtsp_session_prevent_expire (session);
}

static void
link_session_transports (GstRTSPClient * client, GstRTSPSession * session,
    GstRTSPSessionMedia * sessmedia)
{
  guint n_streams, i;

  n_streams =
      gst_rtsp_media_n_streams (gst_rtsp_session_media_get_media (sessmedia));
  for (i = 0; i < n_streams; i++) {
    GstRTSPStreamTransport *trans;
    const GstRTSPTransport *tr;

    /* get the transport, if there is no transport configured, skip this stream */
    trans = gst_rtsp_session_media_get_transport (sessmedia, i);
    if (trans == NULL)
      continue;

    tr = gst_rtsp_stream_transport_get_transport (trans);

    if (tr->lower_transport == GST_RTSP_LOWER_TRANS_TCP) {
      /* for TCP, link the stream to the TCP connection of the client */
      link_transport (client, session, trans);
    }
  }
}

static void
unlink_transport (GstRTSPClient * client, GstRTSPSession * session,
    GstRTSPStreamTransport * trans)
{
  GstRTSPClientPrivate *priv = client->priv;

  GST_DEBUG ("client %p: unlinking transport %p", client, trans);

  gst_rtsp_stream_transport_set_callbacks (trans, NULL, NULL, NULL, NULL);

  priv->transports = g_list_remove (priv->transports, trans);

  /* our session can now expire */
  gst_rtsp_session_allow_expire (session);
}

static void
unlink_session_transports (GstRTSPClient * client, GstRTSPSession * session,
    GstRTSPSessionMedia * sessmedia)
{
  guint n_streams, i;

  n_streams =
      gst_rtsp_media_n_streams (gst_rtsp_session_media_get_media (sessmedia));
  for (i = 0; i < n_streams; i++) {
    GstRTSPStreamTransport *trans;
    const GstRTSPTransport *tr;

    /* get the transport, if there is no transport configured, skip this stream */
    trans = gst_rtsp_session_media_get_transport (sessmedia, i);
    if (trans == NULL)
      continue;

    tr = gst_rtsp_stream_transport_get_transport (trans);

    if (tr->lower_transport == GST_RTSP_LOWER_TRANS_TCP) {
      /* for TCP, unlink the stream from the TCP connection of the client */
      unlink_transport (client, session, trans);
    }
  }
}

static void
close_connection (GstRTSPClient * client)
{
  GstRTSPClientPrivate *priv = client->priv;
  const gchar *tunnelid;

  GST_DEBUG ("client %p: closing connection", client);

  if ((tunnelid = gst_rtsp_connection_get_tunnelid (priv->connection))) {
    g_mutex_lock (&tunnels_lock);
    /* remove from tunnelids */
    g_hash_table_remove (tunnels, tunnelid);
    g_mutex_unlock (&tunnels_lock);
  }

  gst_rtsp_connection_close (priv->connection);
}

static gchar *
default_make_path_from_uri (GstRTSPClient * client, const GstRTSPUrl * uri)
{
  gchar *path;

  if (uri->query)
    path = g_strconcat (uri->abspath, "?", uri->query, NULL);
  else
    path = g_strdup (uri->abspath);

  return path;
}

static gboolean
handle_teardown_request (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPClientPrivate *priv = client->priv;
  GstRTSPClientClass *klass;
  GstRTSPSession *session;
  GstRTSPSessionMedia *sessmedia;
  GstRTSPStatusCode code;
  gchar *path;
  gint matched;

  if (!ctx->session)
    goto no_session;

  session = ctx->session;

  if (!ctx->uri)
    goto no_uri;

  klass = GST_RTSP_CLIENT_GET_CLASS (client);
  path = klass->make_path_from_uri (client, ctx->uri);

  /* get a handle to the configuration of the media in the session */
  sessmedia = gst_rtsp_session_get_media (session, path, &matched);
  if (!sessmedia)
    goto not_found;

  /* only aggregate control for now.. */
  if (path[matched] != '\0')
    goto no_aggregate;

  g_free (path);

  ctx->sessmedia = sessmedia;

  /* we emit the signal before closing the connection */
  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_TEARDOWN_REQUEST],
      0, ctx);

  /* unlink the all TCP callbacks */
  unlink_session_transports (client, session, sessmedia);

  /* remove the session from the watched sessions */
  client_unwatch_session (client, session);

  gst_rtsp_session_media_set_state (sessmedia, GST_STATE_NULL);

  /* unmanage the media in the session, returns false if all media session
   * are torn down. */
  if (!gst_rtsp_session_release_media (session, sessmedia)) {
    /* remove the session */
    gst_rtsp_session_pool_remove (priv->session_pool, session);
  }
  /* construct the response now */
  code = GST_RTSP_STS_OK;
  gst_rtsp_message_init_response (ctx->response, code,
      gst_rtsp_status_as_text (code), ctx->request);

  send_message (client, session, ctx->response, TRUE);

  return TRUE;

  /* ERRORS */
no_session:
  {
    GST_ERROR ("client %p: no session", client);
    send_generic_response (client, GST_RTSP_STS_SESSION_NOT_FOUND, ctx);
    return FALSE;
  }
no_uri:
  {
    GST_ERROR ("client %p: no uri supplied", client);
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, ctx);
    return FALSE;
  }
not_found:
  {
    GST_ERROR ("client %p: no media for uri", client);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, ctx);
    g_free (path);
    return FALSE;
  }
no_aggregate:
  {
    GST_ERROR ("client %p: no aggregate path %s", client, path);
    send_generic_response (client,
        GST_RTSP_STS_ONLY_AGGREGATE_OPERATION_ALLOWED, ctx);
    g_free (path);
    return FALSE;
  }
}

static GstRTSPResult
default_params_set (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPResult res;

  res = gst_rtsp_params_set (client, ctx);

  return res;
}

static GstRTSPResult
default_params_get (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPResult res;

  res = gst_rtsp_params_get (client, ctx);

  return res;
}

static gboolean
default_handle_get_param_request (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPResult res;
  guint8 *data;
  guint size;

  res = gst_rtsp_message_get_body (ctx->request, &data, &size);
  if (res != GST_RTSP_OK)
    goto bad_request;

  if (size == 0) {
    /* no body, keep-alive request */
    send_generic_response (client, GST_RTSP_STS_OK, ctx);
  } else {
    /* there is a body, handle the params */
    res = GST_RTSP_CLIENT_GET_CLASS (client)->params_get (client, ctx);
    if (res != GST_RTSP_OK)
      goto bad_request;

    send_message (client, ctx->session, ctx->response, FALSE);
  }

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_GET_PARAMETER_REQUEST],
      0, ctx);

  return TRUE;

  /* ERRORS */
bad_request:
  {
    GST_ERROR ("client %p: bad request", client);
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, ctx);
    return FALSE;
  }
}

static gboolean
default_handle_set_param_request (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPResult res;
  guint8 *data;
  guint size;

  res = gst_rtsp_message_get_body (ctx->request, &data, &size);
  if (res != GST_RTSP_OK)
    goto bad_request;

  if (size == 0) {
    /* no body, keep-alive request */
    send_generic_response (client, GST_RTSP_STS_OK, ctx);
  } else {
    /* there is a body, handle the params */
    res = GST_RTSP_CLIENT_GET_CLASS (client)->params_set (client, ctx);
    if (res != GST_RTSP_OK)
      goto bad_request;

    send_message (client, ctx->session, ctx->response, FALSE);
  }

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_SET_PARAMETER_REQUEST],
      0, ctx);

  return TRUE;

  /* ERRORS */
bad_request:
  {
    GST_ERROR ("client %p: bad request", client);
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, ctx);
    return FALSE;
  }
}

static gboolean
handle_pause_request (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPSession *session;
  GstRTSPClientClass *klass;
  GstRTSPSessionMedia *sessmedia;
  GstRTSPStatusCode code;
  GstRTSPState rtspstate;
  gchar *path;
  gint matched;

  if (!(session = ctx->session))
    goto no_session;

  if (!ctx->uri)
    goto no_uri;

  klass = GST_RTSP_CLIENT_GET_CLASS (client);
  path = klass->make_path_from_uri (client, ctx->uri);

  /* get a handle to the configuration of the media in the session */
  sessmedia = gst_rtsp_session_get_media (session, path, &matched);
  if (!sessmedia)
    goto not_found;

  if (path[matched] != '\0')
    goto no_aggregate;

  g_free (path);

  ctx->sessmedia = sessmedia;

  rtspstate = gst_rtsp_session_media_get_rtsp_state (sessmedia);
  /* the session state must be playing or recording */
  if (rtspstate != GST_RTSP_STATE_PLAYING &&
      rtspstate != GST_RTSP_STATE_RECORDING)
    goto invalid_state;

  /* unlink the all TCP callbacks */
  unlink_session_transports (client, session, sessmedia);

  /* then pause sending */
  gst_rtsp_session_media_set_state (sessmedia, GST_STATE_PAUSED);

  /* construct the response now */
  code = GST_RTSP_STS_OK;
  gst_rtsp_message_init_response (ctx->response, code,
      gst_rtsp_status_as_text (code), ctx->request);

  send_message (client, session, ctx->response, FALSE);

  /* the state is now READY */
  gst_rtsp_session_media_set_rtsp_state (sessmedia, GST_RTSP_STATE_READY);

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_PAUSE_REQUEST], 0, ctx);

  return TRUE;

  /* ERRORS */
no_session:
  {
    GST_ERROR ("client %p: no seesion", client);
    send_generic_response (client, GST_RTSP_STS_SESSION_NOT_FOUND, ctx);
    return FALSE;
  }
no_uri:
  {
    GST_ERROR ("client %p: no uri supplied", client);
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, ctx);
    return FALSE;
  }
not_found:
  {
    GST_ERROR ("client %p: no media for uri", client);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, ctx);
    g_free (path);
    return FALSE;
  }
no_aggregate:
  {
    GST_ERROR ("client %p: no aggregate path %s", client, path);
    send_generic_response (client,
        GST_RTSP_STS_ONLY_AGGREGATE_OPERATION_ALLOWED, ctx);
    g_free (path);
    return FALSE;
  }
invalid_state:
  {
    GST_ERROR ("client %p: not PLAYING or RECORDING", client);
    send_generic_response (client, GST_RTSP_STS_METHOD_NOT_VALID_IN_THIS_STATE,
        ctx);
    return FALSE;
  }
}

/* convert @url and @path to a URL used as a content base for the factory
 * located at @path */
static gchar *
make_base_url (GstRTSPClient * client, GstRTSPUrl * url, const gchar * path)
{
  GstRTSPUrl tmp;
  gchar *result;
  const gchar *trail;

  /* check for trailing '/' and append one */
  trail = (path[strlen (path) - 1] != '/' ? "/" : "");

  tmp = *url;
  tmp.user = NULL;
  tmp.passwd = NULL;
  tmp.abspath = g_strdup_printf ("%s%s", path, trail);
  tmp.query = NULL;
  result = gst_rtsp_url_get_request_uri (&tmp);
  g_free (tmp.abspath);

  return result;
}

static gboolean
handle_play_request (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPSession *session;
  GstRTSPClientClass *klass;
  GstRTSPSessionMedia *sessmedia;
  GstRTSPMedia *media;
  GstRTSPStatusCode code;
  GstRTSPUrl *uri;
  gchar *str;
  GstRTSPTimeRange *range;
  GstRTSPResult res;
  GstRTSPState rtspstate;
  GstRTSPRangeUnit unit = GST_RTSP_RANGE_NPT;
  gchar *path, *rtpinfo;
  gint matched;

  if (!(session = ctx->session))
    goto no_session;

  if (!(uri = ctx->uri))
    goto no_uri;

  klass = GST_RTSP_CLIENT_GET_CLASS (client);
  path = klass->make_path_from_uri (client, uri);

  /* get a handle to the configuration of the media in the session */
  sessmedia = gst_rtsp_session_get_media (session, path, &matched);
  if (!sessmedia)
    goto not_found;

  if (path[matched] != '\0')
    goto no_aggregate;

  g_free (path);

  ctx->sessmedia = sessmedia;
  ctx->media = media = gst_rtsp_session_media_get_media (sessmedia);

  /* the session state must be playing or ready */
  rtspstate = gst_rtsp_session_media_get_rtsp_state (sessmedia);
  if (rtspstate != GST_RTSP_STATE_PLAYING && rtspstate != GST_RTSP_STATE_READY)
    goto invalid_state;

  /* in play we first unsuspend, media could be suspended from SDP or PAUSED */
  if (!gst_rtsp_media_unsuspend (media))
    goto unsuspend_failed;

  /* parse the range header if we have one */
  res = gst_rtsp_message_get_header (ctx->request, GST_RTSP_HDR_RANGE, &str, 0);
  if (res == GST_RTSP_OK) {
    if (gst_rtsp_range_parse (str, &range) == GST_RTSP_OK) {
      /* we have a range, seek to the position */
      unit = range->unit;
      gst_rtsp_media_seek (media, range);
      gst_rtsp_range_free (range);
    }
  }

  /* link the all TCP callbacks */
  link_session_transports (client, session, sessmedia);

  /* grab RTPInfo from the media now */
  rtpinfo = gst_rtsp_session_media_get_rtpinfo (sessmedia);

  /* construct the response now */
  code = GST_RTSP_STS_OK;
  gst_rtsp_message_init_response (ctx->response, code,
      gst_rtsp_status_as_text (code), ctx->request);

  /* add the RTP-Info header */
  if (rtpinfo)
    gst_rtsp_message_take_header (ctx->response, GST_RTSP_HDR_RTP_INFO,
        rtpinfo);

  /* add the range */
  str = gst_rtsp_media_get_range_string (media, TRUE, unit);
  if (str)
    gst_rtsp_message_take_header (ctx->response, GST_RTSP_HDR_RANGE, str);

  send_message (client, session, ctx->response, FALSE);

  /* start playing after sending the request */
  gst_rtsp_session_media_set_state (sessmedia, GST_STATE_PLAYING);

  gst_rtsp_session_media_set_rtsp_state (sessmedia, GST_RTSP_STATE_PLAYING);

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_PLAY_REQUEST], 0, ctx);

  return TRUE;

  /* ERRORS */
no_session:
  {
    GST_ERROR ("client %p: no session", client);
    send_generic_response (client, GST_RTSP_STS_SESSION_NOT_FOUND, ctx);
    return FALSE;
  }
no_uri:
  {
    GST_ERROR ("client %p: no uri supplied", client);
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, ctx);
    return FALSE;
  }
not_found:
  {
    GST_ERROR ("client %p: media not found", client);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, ctx);
    return FALSE;
  }
no_aggregate:
  {
    GST_ERROR ("client %p: no aggregate path %s", client, path);
    send_generic_response (client,
        GST_RTSP_STS_ONLY_AGGREGATE_OPERATION_ALLOWED, ctx);
    g_free (path);
    return FALSE;
  }
invalid_state:
  {
    GST_ERROR ("client %p: not PLAYING or READY", client);
    send_generic_response (client, GST_RTSP_STS_METHOD_NOT_VALID_IN_THIS_STATE,
        ctx);
    return FALSE;
  }
unsuspend_failed:
  {
    GST_ERROR ("client %p: unsuspend failed", client);
    send_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, ctx);
    return FALSE;
  }
}

static void
do_keepalive (GstRTSPSession * session)
{
  GST_INFO ("keep session %p alive", session);
  gst_rtsp_session_touch (session);
}

/* parse @transport and return a valid transport in @tr. only transports
 * supported by @stream are returned. Returns FALSE if no valid transport
 * was found. */
static gboolean
parse_transport (const char *transport, GstRTSPStream * stream,
    GstRTSPTransport * tr)
{
  gint i;
  gboolean res;
  gchar **transports;

  res = FALSE;
  gst_rtsp_transport_init (tr);

  GST_DEBUG ("parsing transports %s", transport);

  transports = g_strsplit (transport, ",", 0);

  /* loop through the transports, try to parse */
  for (i = 0; transports[i]; i++) {
    res = gst_rtsp_transport_parse (transports[i], tr);
    if (res != GST_RTSP_OK) {
      /* no valid transport, search some more */
      GST_WARNING ("could not parse transport %s", transports[i]);
      goto next;
    }

    /* we have a transport, see if it's supported */
    if (!gst_rtsp_stream_is_transport_supported (stream, tr)) {
      GST_WARNING ("unsupported transport %s", transports[i]);
      goto next;
    }

    /* we have a valid transport */
    GST_INFO ("found valid transport %s", transports[i]);
    res = TRUE;
    break;

  next:
    gst_rtsp_transport_init (tr);
  }
  g_strfreev (transports);

  return res;
}

static gboolean
default_configure_client_media (GstRTSPClient * client, GstRTSPMedia * media,
    GstRTSPStream * stream, GstRTSPContext * ctx)
{
  GstRTSPMessage *request = ctx->request;
  gchar *blocksize_str;

  if (gst_rtsp_message_get_header (request, GST_RTSP_HDR_BLOCKSIZE,
          &blocksize_str, 0) == GST_RTSP_OK) {
    guint64 blocksize;
    gchar *end;

    blocksize = g_ascii_strtoull (blocksize_str, &end, 10);
    if (end == blocksize_str)
      goto parse_failed;

    /* we don't want to change the mtu when this media
     * can be shared because it impacts other clients */
    if (gst_rtsp_media_is_shared (media))
      goto done;

    if (blocksize > G_MAXUINT)
      blocksize = G_MAXUINT;

    gst_rtsp_stream_set_mtu (stream, blocksize);
  }
done:
  return TRUE;

  /* ERRORS */
parse_failed:
  {
    GST_ERROR_OBJECT (client, "failed to parse blocksize");
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, ctx);
    return FALSE;
  }
}

static gboolean
default_configure_client_transport (GstRTSPClient * client,
    GstRTSPContext * ctx, GstRTSPTransport * ct)
{
  GstRTSPClientPrivate *priv = client->priv;

  /* we have a valid transport now, set the destination of the client. */
  if (ct->lower_transport == GST_RTSP_LOWER_TRANS_UDP_MCAST) {
    gboolean use_client_settings;

    use_client_settings =
        gst_rtsp_auth_check (GST_RTSP_AUTH_CHECK_TRANSPORT_CLIENT_SETTINGS);

    if (ct->destination && use_client_settings) {
      GstRTSPAddress *addr;

      addr = gst_rtsp_stream_reserve_address (ctx->stream, ct->destination,
          ct->port.min, ct->port.max - ct->port.min + 1, ct->ttl);

      if (addr == NULL)
        goto no_address;

      gst_rtsp_address_free (addr);
    } else {
      GstRTSPAddress *addr;
      GSocketFamily family;

      family = priv->is_ipv6 ? G_SOCKET_FAMILY_IPV6 : G_SOCKET_FAMILY_IPV4;

      addr = gst_rtsp_stream_get_multicast_address (ctx->stream, family);
      if (addr == NULL)
        goto no_address;

      g_free (ct->destination);
      ct->destination = g_strdup (addr->address);
      ct->port.min = addr->port;
      ct->port.max = addr->port + addr->n_ports - 1;
      ct->ttl = addr->ttl;

      gst_rtsp_address_free (addr);
    }
  } else {
    GstRTSPUrl *url;

    url = gst_rtsp_connection_get_url (priv->connection);
    g_free (ct->destination);
    ct->destination = g_strdup (url->host);

    if (ct->lower_transport & GST_RTSP_LOWER_TRANS_TCP) {
      /* check if the client selected channels for TCP */
      if (ct->interleaved.min == -1 || ct->interleaved.max == -1) {
        gst_rtsp_session_media_alloc_channels (ctx->sessmedia,
            &ct->interleaved);
      }
    }
  }
  return TRUE;

  /* ERRORS */
no_address:
  {
    GST_ERROR_OBJECT (client, "failed to acquire address for stream");
    return FALSE;
  }
}

static GstRTSPTransport *
make_server_transport (GstRTSPClient * client, GstRTSPContext * ctx,
    GstRTSPTransport * ct)
{
  GstRTSPTransport *st;
  GInetAddress *addr;
  GSocketFamily family;

  /* prepare the server transport */
  gst_rtsp_transport_new (&st);

  st->trans = ct->trans;
  st->profile = ct->profile;
  st->lower_transport = ct->lower_transport;

  addr = g_inet_address_new_from_string (ct->destination);

  if (!addr) {
    GST_ERROR ("failed to get inet addr from client destination");
    family = G_SOCKET_FAMILY_IPV4;
  } else {
    family = g_inet_address_get_family (addr);
    g_object_unref (addr);
    addr = NULL;
  }

  switch (st->lower_transport) {
    case GST_RTSP_LOWER_TRANS_UDP:
      st->client_port = ct->client_port;
      gst_rtsp_stream_get_server_port (ctx->stream, &st->server_port, family);
      break;
    case GST_RTSP_LOWER_TRANS_UDP_MCAST:
      st->port = ct->port;
      st->destination = g_strdup (ct->destination);
      st->ttl = ct->ttl;
      break;
    case GST_RTSP_LOWER_TRANS_TCP:
      st->interleaved = ct->interleaved;
    default:
      break;
  }

  gst_rtsp_stream_get_ssrc (ctx->stream, &st->ssrc);

  return st;
}

static gboolean
handle_setup_request (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPClientPrivate *priv = client->priv;
  GstRTSPResult res;
  GstRTSPUrl *uri;
  gchar *transport;
  GstRTSPTransport *ct, *st;
  GstRTSPStatusCode code;
  GstRTSPSession *session;
  GstRTSPStreamTransport *trans;
  gchar *trans_str;
  GstRTSPSessionMedia *sessmedia;
  GstRTSPMedia *media;
  GstRTSPStream *stream;
  GstRTSPState rtspstate;
  GstRTSPClientClass *klass;
  gchar *path, *control;
  gint matched;

  if (!ctx->uri)
    goto no_uri;

  uri = ctx->uri;
  klass = GST_RTSP_CLIENT_GET_CLASS (client);
  path = klass->make_path_from_uri (client, uri);

  /* parse the transport */
  res =
      gst_rtsp_message_get_header (ctx->request, GST_RTSP_HDR_TRANSPORT,
      &transport, 0);
  if (res != GST_RTSP_OK)
    goto no_transport;

  /* we create the session after parsing stuff so that we don't make
   * a session for malformed requests */
  if (priv->session_pool == NULL)
    goto no_pool;

  session = ctx->session;

  if (session) {
    g_object_ref (session);
    /* get a handle to the configuration of the media in the session, this can
     * return NULL if this is a new url to manage in this session. */
    sessmedia = gst_rtsp_session_get_media (session, path, &matched);
  } else {
    /* we need a new media configuration in this session */
    sessmedia = NULL;
  }

  /* we have no session media, find one and manage it */
  if (sessmedia == NULL) {
    /* get a handle to the configuration of the media in the session */
    media = find_media (client, ctx, path, &matched);
  } else {
    if ((media = gst_rtsp_session_media_get_media (sessmedia)))
      g_object_ref (media);
    else
      goto media_not_found;
  }
  /* no media, not found then */
  if (media == NULL)
    goto media_not_found_no_reply;

  /* FIXME-WFD : wfd url problem */
#if 0
  if (path[matched] == '\0')
    goto control_not_found;

  /* path is what matched. */
  path[matched] = '\0';
  /* control is remainder */
  control = &path[matched + 1];
#else
  control = g_strdup("stream=0");
#endif

  /* find the stream now using the control part */
  stream = gst_rtsp_media_find_stream (media, control);
  if (stream == NULL)
    goto stream_not_found;

  /* now we have a uri identifying a valid media and stream */
  ctx->stream = stream;
  ctx->media = media;

  if (session == NULL) {
    /* create a session if this fails we probably reached our session limit or
     * something. */
    if (!(session = gst_rtsp_session_pool_create (priv->session_pool)))
      goto service_unavailable;

    /* make sure this client is closed when the session is closed */
    client_watch_session (client, session);

    /* signal new session */
    g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_NEW_SESSION], 0,
        session);

    ctx->session = session;
  }

  if (sessmedia == NULL) {
    /* manage the media in our session now, if not done already  */
    sessmedia = gst_rtsp_session_manage_media (session, path, media);
    /* if we stil have no media, error */
    if (sessmedia == NULL)
      goto sessmedia_unavailable;
  } else {
    g_object_unref (media);
  }

  ctx->sessmedia = sessmedia;

  if (!klass->configure_client_media (client, media, stream, ctx))
    goto configure_media_failed_no_reply;

  gst_rtsp_transport_new (&ct);

  /* parse and find a usable supported transport */
  if (!parse_transport (transport, stream, ct))
    goto unsupported_transports;

  /* update the client transport */
  if (!klass->configure_client_transport (client, ctx, ct))
    goto unsupported_client_transport;

  /* set in the session media transport */
  trans = gst_rtsp_session_media_set_transport (sessmedia, stream, ct);

  /* configure the url used to set this transport, this we will use when
   * generating the response for the PLAY request */
  gst_rtsp_stream_transport_set_url (trans, uri);

  /* configure keepalive for this transport */
  gst_rtsp_stream_transport_set_keepalive (trans,
      (GstRTSPKeepAliveFunc) do_keepalive, session, NULL);

  /* create and serialize the server transport */
  st = make_server_transport (client, ctx, ct);
  trans_str = gst_rtsp_transport_as_text (st);

  /* FIXME-WFD : Temporarily force to set profile string */
  trans_str = g_strjoinv("RTP/AVP/UDP", g_strsplit(trans_str, "RTP/AVP", -1));

  gst_rtsp_transport_free (st);

  /* construct the response now */
  code = GST_RTSP_STS_OK;
  gst_rtsp_message_init_response (ctx->response, code,
      gst_rtsp_status_as_text (code), ctx->request);

  gst_rtsp_message_add_header (ctx->response, GST_RTSP_HDR_TRANSPORT,
      trans_str);
  g_free (trans_str);

  send_message (client, session, ctx->response, FALSE);

  /* update the state */
  rtspstate = gst_rtsp_session_media_get_rtsp_state (sessmedia);
  switch (rtspstate) {
    case GST_RTSP_STATE_PLAYING:
    case GST_RTSP_STATE_RECORDING:
    case GST_RTSP_STATE_READY:
      /* no state change */
      break;
    default:
      gst_rtsp_session_media_set_rtsp_state (sessmedia, GST_RTSP_STATE_READY);
      break;
  }
  g_object_unref (session);
  g_free (path);

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_SETUP_REQUEST], 0, ctx);

  return TRUE;

  /* ERRORS */
no_uri:
  {
    GST_ERROR ("client %p: no uri", client);
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, ctx);
    return FALSE;
  }
no_transport:
  {
    GST_ERROR ("client %p: no transport", client);
    send_generic_response (client, GST_RTSP_STS_UNSUPPORTED_TRANSPORT, ctx);
    g_free (path);
    return FALSE;
  }
no_pool:
  {
    GST_ERROR ("client %p: no session pool configured", client);
    send_generic_response (client, GST_RTSP_STS_SESSION_NOT_FOUND, ctx);
    g_free (path);
    return FALSE;
  }
media_not_found_no_reply:
  {
    GST_ERROR ("client %p: media '%s' not found", client, path);
    g_free (path);
    /* error reply is already sent */
    return FALSE;
  }
media_not_found:
  {
    GST_ERROR ("client %p: media '%s' not found", client, path);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, ctx);
    g_free (path);
    return FALSE;
  }
control_not_found:
  {
    GST_ERROR ("client %p: no control in path '%s'", client, path);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, ctx);
    g_object_unref (media);
    g_free (path);
    return FALSE;
  }
stream_not_found:
  {
    GST_ERROR ("client %p: stream '%s' not found", client, control);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, ctx);
    g_object_unref (media);
    g_free (path);
    return FALSE;
  }
service_unavailable:
  {
    GST_ERROR ("client %p: can't create session", client);
    send_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, ctx);
    g_object_unref (media);
    g_free (path);
    return FALSE;
  }
sessmedia_unavailable:
  {
    GST_ERROR ("client %p: can't create session media", client);
    send_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, ctx);
    g_object_unref (media);
    g_object_unref (session);
    g_free (path);
    return FALSE;
  }
configure_media_failed_no_reply:
  {
    GST_ERROR ("client %p: configure_media failed", client);
    g_object_unref (session);
    g_free (path);
    /* error reply is already sent */
    return FALSE;
  }
unsupported_transports:
  {
    GST_ERROR ("client %p: unsupported transports", client);
    send_generic_response (client, GST_RTSP_STS_UNSUPPORTED_TRANSPORT, ctx);
    gst_rtsp_transport_free (ct);
    g_object_unref (session);
    g_free (path);
    return FALSE;
  }
unsupported_client_transport:
  {
    GST_ERROR ("client %p: unsupported client transport", client);
    send_generic_response (client, GST_RTSP_STS_UNSUPPORTED_TRANSPORT, ctx);
    gst_rtsp_transport_free (ct);
    g_object_unref (session);
    g_free (path);
    return FALSE;
  }
}

static GstSDPMessage *
create_sdp (GstRTSPClient * client, GstRTSPMedia * media)
{
  GstRTSPClientPrivate *priv = client->priv;
  GstSDPMessage *sdp;
  GstSDPInfo info;
  const gchar *proto;

  gst_sdp_message_new (&sdp);

  /* some standard things first */
  gst_sdp_message_set_version (sdp, "0");

  if (priv->is_ipv6)
    proto = "IP6";
  else
    proto = "IP4";

  gst_sdp_message_set_origin (sdp, "-", "1188340656180883", "1", "IN", proto,
      priv->server_ip);

  gst_sdp_message_set_session_name (sdp, "Session streamed with GStreamer");
  gst_sdp_message_set_information (sdp, "rtsp-server");
  gst_sdp_message_add_time (sdp, "0", "0", NULL);
  gst_sdp_message_add_attribute (sdp, "tool", "GStreamer");
  gst_sdp_message_add_attribute (sdp, "type", "broadcast");
  gst_sdp_message_add_attribute (sdp, "control", "*");

  info.is_ipv6 = priv->is_ipv6;
  info.server_ip = priv->server_ip;

  /* create an SDP for the media object */
  if (!gst_rtsp_media_setup_sdp (media, sdp, &info))
    goto no_sdp;

  return sdp;

  /* ERRORS */
no_sdp:
  {
    GST_ERROR ("client %p: could not create SDP", client);
    gst_sdp_message_free (sdp);
    return NULL;
  }
}

/* for the describe we must generate an SDP */
static gboolean
handle_describe_request (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPClientPrivate *priv = client->priv;
  GstRTSPResult res;
  GstSDPMessage *sdp;
  guint i;
  gchar *path, *str;
  GstRTSPMedia *media;
  GstRTSPClientClass *klass;

  klass = GST_RTSP_CLIENT_GET_CLASS (client);

  if (!ctx->uri)
    goto no_uri;

  /* check what kind of format is accepted, we don't really do anything with it
   * and always return SDP for now. */
  for (i = 0;; i++) {
    gchar *accept;

    res =
        gst_rtsp_message_get_header (ctx->request, GST_RTSP_HDR_ACCEPT,
        &accept, i);
    if (res == GST_RTSP_ENOTIMPL)
      break;

    if (g_ascii_strcasecmp (accept, "application/sdp") == 0)
      break;
  }

  if (!priv->mount_points)
    goto no_mount_points;

  if (!(path = gst_rtsp_mount_points_make_path (priv->mount_points, ctx->uri)))
    goto no_path;

  /* find the media object for the uri */
  if (!(media = find_media (client, ctx, path, NULL)))
    goto no_media;

  /* create an SDP for the media object on this client */
  if (!(sdp = klass->create_sdp (client, media)))
    goto no_sdp;

  /* we suspend after the describe */
  gst_rtsp_media_suspend (media);
  g_object_unref (media);

  gst_rtsp_message_init_response (ctx->response, GST_RTSP_STS_OK,
      gst_rtsp_status_as_text (GST_RTSP_STS_OK), ctx->request);

  gst_rtsp_message_add_header (ctx->response, GST_RTSP_HDR_CONTENT_TYPE,
      "application/sdp");

  /* content base for some clients that might screw up creating the setup uri */
  str = make_base_url (client, ctx->uri, path);
  g_free (path);

  GST_INFO ("adding content-base: %s", str);
  gst_rtsp_message_take_header (ctx->response, GST_RTSP_HDR_CONTENT_BASE, str);

  /* add SDP to the response body */
  str = gst_sdp_message_as_text (sdp);
  gst_rtsp_message_take_body (ctx->response, (guint8 *) str, strlen (str));
  gst_sdp_message_free (sdp);

  send_message (client, ctx->session, ctx->response, FALSE);

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_DESCRIBE_REQUEST],
      0, ctx);

  return TRUE;

  /* ERRORS */
no_uri:
  {
    GST_ERROR ("client %p: no uri", client);
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, ctx);
    return FALSE;
  }
no_mount_points:
  {
    GST_ERROR ("client %p: no mount points configured", client);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, ctx);
    return FALSE;
  }
no_path:
  {
    GST_ERROR ("client %p: can't find path for url", client);
    send_generic_response (client, GST_RTSP_STS_NOT_FOUND, ctx);
    return FALSE;
  }
no_media:
  {
    GST_ERROR ("client %p: no media", client);
    g_free (path);
    /* error reply is already sent */
    return FALSE;
  }
no_sdp:
  {
    GST_ERROR ("client %p: can't create SDP", client);
    send_generic_response (client, GST_RTSP_STS_SERVICE_UNAVAILABLE, ctx);
    g_free (path);
    g_object_unref (media);
    return FALSE;
  }
}

static gboolean
default_handle_options_request (GstRTSPClient * client, GstRTSPContext * ctx)
{
  GstRTSPMethod options;
  gchar *str;

  options = GST_RTSP_DESCRIBE |
      GST_RTSP_OPTIONS |
      GST_RTSP_PAUSE |
      GST_RTSP_PLAY |
      GST_RTSP_SETUP |
      GST_RTSP_GET_PARAMETER | GST_RTSP_SET_PARAMETER | GST_RTSP_TEARDOWN;

  str = gst_rtsp_options_as_text (options);

  gst_rtsp_message_init_response (ctx->response, GST_RTSP_STS_OK,
      gst_rtsp_status_as_text (GST_RTSP_STS_OK), ctx->request);

  gst_rtsp_message_add_header (ctx->response, GST_RTSP_HDR_PUBLIC, str);
  g_free (str);

  send_message (client, ctx->session, ctx->response, FALSE);

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_OPTIONS_REQUEST],
      0, ctx);

  return TRUE;
}

/* remove duplicate and trailing '/' */
static void
sanitize_uri (GstRTSPUrl * uri)
{
  gint i, len;
  gchar *s, *d;
  gboolean have_slash, prev_slash;

  s = d = uri->abspath;
  len = strlen (uri->abspath);

  prev_slash = FALSE;

  for (i = 0; i < len; i++) {
    have_slash = s[i] == '/';
    *d = s[i];
    if (!have_slash || !prev_slash)
      d++;
    prev_slash = have_slash;
  }
  len = d - uri->abspath;
  /* don't remove the first slash if that's the only thing left */
  if (len > 1 && *(d - 1) == '/')
    d--;
  *d = '\0';
}

static void
client_session_finalized (GstRTSPClient * client, GstRTSPSession * session)
{
  GstRTSPClientPrivate *priv = client->priv;

  GST_INFO ("client %p: session %p finished", client, session);

  /* unlink all media managed in this session */
  client_unlink_session (client, session);

  /* remove the session */
  if (!(priv->sessions = g_list_remove (priv->sessions, session))) {
    GST_INFO ("client %p: all sessions finalized, close the connection",
        client);
    close_connection (client);
  }
}

static void
handle_request (GstRTSPClient * client, GstRTSPMessage * request)
{
  GstRTSPClientPrivate *priv = client->priv;
  GstRTSPMethod method;
  const gchar *uristr;
  GstRTSPUrl *uri = NULL;
  GstRTSPVersion version;
  GstRTSPResult res;
  GstRTSPSession *session = NULL;
  GstRTSPContext sctx = { NULL }, *ctx;
  GstRTSPMessage response = { 0 };
  gchar *sessid;
  GstRTSPClientClass *klass;

  klass = GST_RTSP_CLIENT_GET_CLASS (client);

  if (!(ctx = gst_rtsp_context_get_current ())) {
    ctx = &sctx;
    ctx->auth = priv->auth;
    gst_rtsp_context_push_current (ctx);
  }

  ctx->conn = priv->connection;
  ctx->client = client;
  ctx->request = request;
  ctx->response = &response;

  if (gst_debug_category_get_threshold (rtsp_client_debug) >= GST_LEVEL_LOG) {
    gst_rtsp_message_dump (request);
  }

  gst_rtsp_message_parse_request (request, &method, &uristr, &version);

  GST_INFO ("client %p: received a request %s %s %s", client,
      gst_rtsp_method_as_text (method), uristr,
      gst_rtsp_version_as_text (version));

  /* we can only handle 1.0 requests */
  if (version != GST_RTSP_VERSION_1_0)
    goto not_supported;

  ctx->method = method;

  /* we always try to parse the url first */
  if (strcmp (uristr, "*") == 0) {
    /* special case where we have * as uri, keep uri = NULL */
  } else if (gst_rtsp_url_parse (uristr, &uri) != GST_RTSP_OK) {
    /* check if the uristr is an absolute path <=> scheme and host information
     * is missing */
    gchar *scheme;

    scheme = g_uri_parse_scheme (uristr);
    if (scheme == NULL && g_str_has_prefix (uristr, "/")) {
      gchar *absolute_uristr = NULL;

      GST_WARNING_OBJECT (client, "request doesn't contain absolute url");
      if (priv->server_ip == NULL) {
        GST_WARNING_OBJECT (client, "host information missing");
        goto bad_request;
      }

      absolute_uristr =
          g_strdup_printf ("rtsp://%s%s", priv->server_ip, uristr);

      GST_DEBUG_OBJECT (client, "absolute url: %s", absolute_uristr);
      if (gst_rtsp_url_parse (absolute_uristr, &uri) != GST_RTSP_OK) {
        g_free (absolute_uristr);
        goto bad_request;
      }
      g_free (absolute_uristr);
    } else {
      g_free (scheme);
      goto bad_request;
    }
  }

  /* get the session if there is any */
  res = gst_rtsp_message_get_header (request, GST_RTSP_HDR_SESSION, &sessid, 0);
  if (res == GST_RTSP_OK) {
    if (priv->session_pool == NULL)
      goto no_pool;

    /* we had a session in the request, find it again */
    if (!(session = gst_rtsp_session_pool_find (priv->session_pool, sessid)))
      goto session_not_found;

    /* we add the session to the client list of watched sessions. When a session
     * disappears because it times out, we will be notified. If all sessions are
     * gone, we will close the connection */
    client_watch_session (client, session);
  }

  /* sanitize the uri */
  if (uri)
    sanitize_uri (uri);
  ctx->uri = uri;
  ctx->session = session;

  if (!gst_rtsp_auth_check (GST_RTSP_AUTH_CHECK_URL))
    goto not_authorized;

  /* now see what is asked and dispatch to a dedicated handler */
  switch (method) {
    case GST_RTSP_OPTIONS:
      klass->handle_options_request (client, ctx);
      break;
    case GST_RTSP_DESCRIBE:
      handle_describe_request (client, ctx);
      break;
    case GST_RTSP_SETUP:
      handle_setup_request (client, ctx);
      break;
    case GST_RTSP_PLAY:
      handle_play_request (client, ctx);
      break;
    case GST_RTSP_PAUSE:
      handle_pause_request (client, ctx);
      break;
    case GST_RTSP_TEARDOWN:
      handle_teardown_request (client, ctx);
      break;
    case GST_RTSP_SET_PARAMETER:
      klass->handle_set_param_request (client, ctx);
      break;
    case GST_RTSP_GET_PARAMETER:
      klass->handle_get_param_request (client, ctx);
      break;
    case GST_RTSP_ANNOUNCE:
    case GST_RTSP_RECORD:
    case GST_RTSP_REDIRECT:
      goto not_implemented;
    case GST_RTSP_INVALID:
    default:
      goto bad_request;
  }

done:
  if (ctx == &sctx)
    gst_rtsp_context_pop_current (ctx);
  if (session)
    g_object_unref (session);
  if (uri)
    gst_rtsp_url_free (uri);
  return;

  /* ERRORS */
not_supported:
  {
    GST_ERROR ("client %p: version %d not supported", client, version);
    send_generic_response (client, GST_RTSP_STS_RTSP_VERSION_NOT_SUPPORTED,
        ctx);
    goto done;
  }
bad_request:
  {
    GST_ERROR ("client %p: bad request", client);
    send_generic_response (client, GST_RTSP_STS_BAD_REQUEST, ctx);
    goto done;
  }
no_pool:
  {
    GST_ERROR ("client %p: no pool configured", client);
    send_generic_response (client, GST_RTSP_STS_SESSION_NOT_FOUND, ctx);
    goto done;
  }
session_not_found:
  {
    GST_ERROR ("client %p: session not found", client);
    send_generic_response (client, GST_RTSP_STS_SESSION_NOT_FOUND, ctx);
    goto done;
  }
not_authorized:
  {
    GST_ERROR ("client %p: not allowed", client);
    /* error reply is already sent */
    goto done;
  }
not_implemented:
  {
    GST_ERROR ("client %p: method %d not implemented", client, method);
    send_generic_response (client, GST_RTSP_STS_NOT_IMPLEMENTED, ctx);
    goto done;
  }
}


static void
handle_response (GstRTSPClient * client, GstRTSPMessage * response)
{
  GstRTSPClientPrivate *priv = client->priv;
  GstRTSPResult res;
  GstRTSPSession *session = NULL;
  GstRTSPContext sctx = { NULL }, *ctx;
  gchar *sessid;

  if (!(ctx = gst_rtsp_context_get_current ())) {
    ctx = &sctx;
    ctx->auth = priv->auth;
    gst_rtsp_context_push_current (ctx);
  }

  ctx->conn = priv->connection;
  ctx->client = client;
  ctx->request = NULL;
  ctx->uri = NULL;
  ctx->method = GST_RTSP_INVALID;
  ctx->response = response;

  if (gst_debug_category_get_threshold (rtsp_client_debug) >= GST_LEVEL_LOG) {
    gst_rtsp_message_dump (response);
  }

  GST_INFO ("client %p: received a response", client);

  /* get the session if there is any */
  res =
      gst_rtsp_message_get_header (response, GST_RTSP_HDR_SESSION, &sessid, 0);
  if (res == GST_RTSP_OK) {
    if (priv->session_pool == NULL)
      goto no_pool;

    /* we had a session in the request, find it again */
    if (!(session = gst_rtsp_session_pool_find (priv->session_pool, sessid)))
      goto session_not_found;

    /* we add the session to the client list of watched sessions. When a session
     * disappears because it times out, we will be notified. If all sessions are
     * gone, we will close the connection */
    client_watch_session (client, session);
  }

  ctx->session = session;

  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_HANDLE_RESPONSE],
      0, ctx);

done:
  if (ctx == &sctx)
    gst_rtsp_context_pop_current (ctx);
  if (session)
    g_object_unref (session);
  return;

no_pool:
  {
    GST_ERROR ("client %p: no pool configured", client);
    goto done;
  }
session_not_found:
  {
    GST_ERROR ("client %p: session not found", client);
    goto done;
  }
}

static void
handle_data (GstRTSPClient * client, GstRTSPMessage * message)
{
  GstRTSPClientPrivate *priv = client->priv;
  GstRTSPResult res;
  guint8 channel;
  GList *walk;
  guint8 *data;
  guint size;
  GstBuffer *buffer;
  gboolean handled;

  /* find the stream for this message */
  res = gst_rtsp_message_parse_data (message, &channel);
  if (res != GST_RTSP_OK)
    return;

  gst_rtsp_message_steal_body (message, &data, &size);

  buffer = gst_buffer_new_wrapped (data, size);

  handled = FALSE;
  for (walk = priv->transports; walk; walk = g_list_next (walk)) {
    GstRTSPStreamTransport *trans;
    GstRTSPStream *stream;
    const GstRTSPTransport *tr;

    trans = walk->data;

    tr = gst_rtsp_stream_transport_get_transport (trans);
    stream = gst_rtsp_stream_transport_get_stream (trans);

    /* check for TCP transport */
    if (tr->lower_transport == GST_RTSP_LOWER_TRANS_TCP) {
      /* dispatch to the stream based on the channel number */
      if (tr->interleaved.min == channel) {
        gst_rtsp_stream_recv_rtp (stream, buffer);
        handled = TRUE;
        break;
      } else if (tr->interleaved.max == channel) {
        gst_rtsp_stream_recv_rtcp (stream, buffer);
        handled = TRUE;
        break;
      }
    }
  }
  if (!handled)
    gst_buffer_unref (buffer);
}

/**
 * gst_rtsp_client_set_session_pool:
 * @client: a #GstRTSPClient
 * @pool: a #GstRTSPSessionPool
 *
 * Set @pool as the sessionpool for @client which it will use to find
 * or allocate sessions. the sessionpool is usually inherited from the server
 * that created the client but can be overridden later.
 */
void
gst_rtsp_client_set_session_pool (GstRTSPClient * client,
    GstRTSPSessionPool * pool)
{
  GstRTSPSessionPool *old;
  GstRTSPClientPrivate *priv;

  g_return_if_fail (GST_IS_RTSP_CLIENT (client));

  priv = client->priv;

  if (pool)
    g_object_ref (pool);

  g_mutex_lock (&priv->lock);
  old = priv->session_pool;
  priv->session_pool = pool;
  g_mutex_unlock (&priv->lock);

  if (old)
    g_object_unref (old);
}

/**
 * gst_rtsp_client_get_session_pool:
 * @client: a #GstRTSPClient
 *
 * Get the #GstRTSPSessionPool object that @client uses to manage its sessions.
 *
 * Returns: (transfer full): a #GstRTSPSessionPool, unref after usage.
 */
GstRTSPSessionPool *
gst_rtsp_client_get_session_pool (GstRTSPClient * client)
{
  GstRTSPClientPrivate *priv;
  GstRTSPSessionPool *result;

  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), NULL);

  priv = client->priv;

  g_mutex_lock (&priv->lock);
  if ((result = priv->session_pool))
    g_object_ref (result);
  g_mutex_unlock (&priv->lock);

  return result;
}

/**
 * gst_rtsp_client_set_mount_points:
 * @client: a #GstRTSPClient
 * @mounts: a #GstRTSPMountPoints
 *
 * Set @mounts as the mount points for @client which it will use to map urls
 * to media streams. These mount points are usually inherited from the server that
 * created the client but can be overriden later.
 */
void
gst_rtsp_client_set_mount_points (GstRTSPClient * client,
    GstRTSPMountPoints * mounts)
{
  GstRTSPClientPrivate *priv;
  GstRTSPMountPoints *old;

  g_return_if_fail (GST_IS_RTSP_CLIENT (client));

  priv = client->priv;

  if (mounts)
    g_object_ref (mounts);

  g_mutex_lock (&priv->lock);
  old = priv->mount_points;
  priv->mount_points = mounts;
  g_mutex_unlock (&priv->lock);

  if (old)
    g_object_unref (old);
}

/**
 * gst_rtsp_client_get_mount_points:
 * @client: a #GstRTSPClient
 *
 * Get the #GstRTSPMountPoints object that @client uses to manage its sessions.
 *
 * Returns: (transfer full): a #GstRTSPMountPoints, unref after usage.
 */
GstRTSPMountPoints *
gst_rtsp_client_get_mount_points (GstRTSPClient * client)
{
  GstRTSPClientPrivate *priv;
  GstRTSPMountPoints *result;

  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), NULL);

  priv = client->priv;

  g_mutex_lock (&priv->lock);
  if ((result = priv->mount_points))
    g_object_ref (result);
  g_mutex_unlock (&priv->lock);

  return result;
}

/**
 * gst_rtsp_client_set_auth:
 * @client: a #GstRTSPClient
 * @auth: a #GstRTSPAuth
 *
 * configure @auth to be used as the authentication manager of @client.
 */
void
gst_rtsp_client_set_auth (GstRTSPClient * client, GstRTSPAuth * auth)
{
  GstRTSPClientPrivate *priv;
  GstRTSPAuth *old;

  g_return_if_fail (GST_IS_RTSP_CLIENT (client));

  priv = client->priv;

  if (auth)
    g_object_ref (auth);

  g_mutex_lock (&priv->lock);
  old = priv->auth;
  priv->auth = auth;
  g_mutex_unlock (&priv->lock);

  if (old)
    g_object_unref (old);
}


/**
 * gst_rtsp_client_get_auth:
 * @client: a #GstRTSPClient
 *
 * Get the #GstRTSPAuth used as the authentication manager of @client.
 *
 * Returns: (transfer full): the #GstRTSPAuth of @client. g_object_unref() after
 * usage.
 */
GstRTSPAuth *
gst_rtsp_client_get_auth (GstRTSPClient * client)
{
  GstRTSPClientPrivate *priv;
  GstRTSPAuth *result;

  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), NULL);

  priv = client->priv;

  g_mutex_lock (&priv->lock);
  if ((result = priv->auth))
    g_object_ref (result);
  g_mutex_unlock (&priv->lock);

  return result;
}

/**
 * gst_rtsp_client_set_thread_pool:
 * @client: a #GstRTSPClient
 * @pool: a #GstRTSPThreadPool
 *
 * configure @pool to be used as the thread pool of @client.
 */
void
gst_rtsp_client_set_thread_pool (GstRTSPClient * client,
    GstRTSPThreadPool * pool)
{
  GstRTSPClientPrivate *priv;
  GstRTSPThreadPool *old;

  g_return_if_fail (GST_IS_RTSP_CLIENT (client));

  priv = client->priv;

  if (pool)
    g_object_ref (pool);

  g_mutex_lock (&priv->lock);
  old = priv->thread_pool;
  priv->thread_pool = pool;
  g_mutex_unlock (&priv->lock);

  if (old)
    g_object_unref (old);
}

/**
 * gst_rtsp_client_get_thread_pool:
 * @client: a #GstRTSPClient
 *
 * Get the #GstRTSPThreadPool used as the thread pool of @client.
 *
 * Returns: (transfer full): the #GstRTSPThreadPool of @client. g_object_unref() after
 * usage.
 */
GstRTSPThreadPool *
gst_rtsp_client_get_thread_pool (GstRTSPClient * client)
{
  GstRTSPClientPrivate *priv;
  GstRTSPThreadPool *result;

  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), NULL);

  priv = client->priv;

  g_mutex_lock (&priv->lock);
  if ((result = priv->thread_pool))
    g_object_ref (result);
  g_mutex_unlock (&priv->lock);

  return result;
}

/**
 * gst_rtsp_client_set_connection:
 * @client: a #GstRTSPClient
 * @conn: (transfer full): a #GstRTSPConnection
 *
 * Set the #GstRTSPConnection of @client. This function takes ownership of
 * @conn.
 *
 * Returns: %TRUE on success.
 */
gboolean
gst_rtsp_client_set_connection (GstRTSPClient * client,
    GstRTSPConnection * conn)
{
  GstRTSPClientPrivate *priv;
  GSocket *read_socket;
  GSocketAddress *address;
  GstRTSPUrl *url;
  GError *error = NULL;

  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), FALSE);
  g_return_val_if_fail (conn != NULL, FALSE);

  priv = client->priv;

  read_socket = gst_rtsp_connection_get_read_socket (conn);

  if (!(address = g_socket_get_local_address (read_socket, &error)))
    goto no_address;

  g_free (priv->server_ip);
  /* keep the original ip that the client connected to */
  if (G_IS_INET_SOCKET_ADDRESS (address)) {
    GInetAddress *iaddr;

    iaddr = g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (address));

    /* socket might be ipv6 but adress still ipv4 */
    priv->is_ipv6 = g_inet_address_get_family (iaddr) == G_SOCKET_FAMILY_IPV6;
    priv->server_ip = g_inet_address_to_string (iaddr);
    g_object_unref (address);
  } else {
    priv->is_ipv6 = g_socket_get_family (read_socket) == G_SOCKET_FAMILY_IPV6;
    priv->server_ip = g_strdup ("unknown");
  }

  GST_INFO ("client %p connected to server ip %s, ipv6 = %d", client,
      priv->server_ip, priv->is_ipv6);

  url = gst_rtsp_connection_get_url (conn);
  GST_INFO ("added new client %p ip %s:%d", client, url->host, url->port);

  priv->connection = conn;

  return TRUE;

  /* ERRORS */
no_address:
  {
    GST_ERROR ("could not get local address %s", error->message);
    g_error_free (error);
    return FALSE;
  }
}

/**
 * gst_rtsp_client_get_connection:
 * @client: a #GstRTSPClient
 *
 * Get the #GstRTSPConnection of @client.
 *
 * Returns: (transfer none): the #GstRTSPConnection of @client.
 * The connection object returned remains valid until the client is freed.
 */
GstRTSPConnection *
gst_rtsp_client_get_connection (GstRTSPClient * client)
{
  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), NULL);

  return client->priv->connection;
}

/**
 * gst_rtsp_client_set_send_func:
 * @client: a #GstRTSPClient
 * @func: a #GstRTSPClientSendFunc
 * @user_data: user data passed to @func
 * @notify: called when @user_data is no longer in use
 *
 * Set @func as the callback that will be called when a new message needs to be
 * sent to the client. @user_data is passed to @func and @notify is called when
 * @user_data is no longer in use.
 *
 * By default, the client will send the messages on the #GstRTSPConnection that
 * was configured with gst_rtsp_client_attach() was called.
 */
void
gst_rtsp_client_set_send_func (GstRTSPClient * client,
    GstRTSPClientSendFunc func, gpointer user_data, GDestroyNotify notify)
{
  GstRTSPClientPrivate *priv;
  GDestroyNotify old_notify;
  gpointer old_data;

  g_return_if_fail (GST_IS_RTSP_CLIENT (client));

  priv = client->priv;

  g_mutex_lock (&priv->send_lock);
  priv->send_func = func;
  old_notify = priv->send_notify;
  old_data = priv->send_data;
  priv->send_notify = notify;
  priv->send_data = user_data;
  g_mutex_unlock (&priv->send_lock);

  if (old_notify)
    old_notify (old_data);
}

/**
 * gst_rtsp_client_handle_message:
 * @client: a #GstRTSPClient
 * @message: an #GstRTSPMessage
 *
 * Let the client handle @message.
 *
 * Returns: a #GstRTSPResult.
 */
GstRTSPResult
gst_rtsp_client_handle_message (GstRTSPClient * client,
    GstRTSPMessage * message)
{
  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), GST_RTSP_EINVAL);
  g_return_val_if_fail (message != NULL, GST_RTSP_EINVAL);

  switch (message->type) {
    case GST_RTSP_MESSAGE_REQUEST:
      handle_request (client, message);
      break;
    case GST_RTSP_MESSAGE_RESPONSE:
      handle_response (client, message);
      break;
    case GST_RTSP_MESSAGE_DATA:
      handle_data (client, message);
      break;
    default:
      break;
  }
  return GST_RTSP_OK;
}

/**
 * gst_rtsp_client_send_message:
 * @client: a #GstRTSPClient
 * @session: a #GstRTSPSession to send the message to or %NULL
 * @message: The #GstRTSPMessage to send
 *
 * Send a message message to the remote end. @message must be a
 * #GST_RTSP_MESSAGE_REQUEST or a #GST_RTSP_MESSAGE_RESPONSE.
 */
GstRTSPResult
gst_rtsp_client_send_message (GstRTSPClient * client, GstRTSPSession * session,
    GstRTSPMessage * message)
{
  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), GST_RTSP_EINVAL);
  g_return_val_if_fail (message != NULL, GST_RTSP_EINVAL);
  g_return_val_if_fail (message->type == GST_RTSP_MESSAGE_REQUEST ||
      message->type == GST_RTSP_MESSAGE_RESPONSE, GST_RTSP_EINVAL);

  send_message (client, session, message, FALSE);

  return GST_RTSP_OK;
}

static GstRTSPResult
do_send_message (GstRTSPClient * client, GstRTSPMessage * message,
    gboolean close, gpointer user_data)
{
  GstRTSPClientPrivate *priv = client->priv;

  /* send the response and store the seq number so we can wait until it's
   * written to the client to close the connection */
  return gst_rtsp_watch_send_message (priv->watch, message, close ?
      &priv->close_seq : NULL);
}

static GstRTSPResult
message_received (GstRTSPWatch * watch, GstRTSPMessage * message,
    gpointer user_data)
{
  return gst_rtsp_client_handle_message (GST_RTSP_CLIENT (user_data), message);
}

static GstRTSPResult
message_sent (GstRTSPWatch * watch, guint cseq, gpointer user_data)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (user_data);
  GstRTSPClientPrivate *priv = client->priv;

  if (priv->close_seq && priv->close_seq == cseq) {
    priv->close_seq = 0;
    close_connection (client);
  }

  return GST_RTSP_OK;
}

static GstRTSPResult
closed (GstRTSPWatch * watch, gpointer user_data)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (user_data);
  GstRTSPClientPrivate *priv = client->priv;
  const gchar *tunnelid;

  GST_INFO ("client %p: connection closed", client);

  if ((tunnelid = gst_rtsp_connection_get_tunnelid (priv->connection))) {
    g_mutex_lock (&tunnels_lock);
    /* remove from tunnelids */
    g_hash_table_remove (tunnels, tunnelid);
    g_mutex_unlock (&tunnels_lock);
  }

  gst_rtsp_client_set_send_func (client, NULL, NULL, NULL);

  return GST_RTSP_OK;
}

static GstRTSPResult
error (GstRTSPWatch * watch, GstRTSPResult result, gpointer user_data)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (user_data);
  gchar *str;

  str = gst_rtsp_strresult (result);
  GST_INFO ("client %p: received an error %s", client, str);
  g_free (str);

  return GST_RTSP_OK;
}

static GstRTSPResult
error_full (GstRTSPWatch * watch, GstRTSPResult result,
    GstRTSPMessage * message, guint id, gpointer user_data)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (user_data);
  gchar *str;

  str = gst_rtsp_strresult (result);
  GST_INFO
      ("client %p: error when handling message %p with id %d: %s",
      client, message, id, str);
  g_free (str);

  return GST_RTSP_OK;
}

static gboolean
remember_tunnel (GstRTSPClient * client)
{
  GstRTSPClientPrivate *priv = client->priv;
  const gchar *tunnelid;

  /* store client in the pending tunnels */
  tunnelid = gst_rtsp_connection_get_tunnelid (priv->connection);
  if (tunnelid == NULL)
    goto no_tunnelid;

  GST_INFO ("client %p: inserting tunnel session %s", client, tunnelid);

  /* we can't have two clients connecting with the same tunnelid */
  g_mutex_lock (&tunnels_lock);
  if (g_hash_table_lookup (tunnels, tunnelid))
    goto tunnel_existed;

  g_hash_table_insert (tunnels, g_strdup (tunnelid), g_object_ref (client));
  g_mutex_unlock (&tunnels_lock);

  return TRUE;

  /* ERRORS */
no_tunnelid:
  {
    GST_ERROR ("client %p: no tunnelid provided", client);
    return FALSE;
  }
tunnel_existed:
  {
    g_mutex_unlock (&tunnels_lock);
    GST_ERROR ("client %p: tunnel session %s already existed", client,
        tunnelid);
    return FALSE;
  }
}

static GstRTSPStatusCode
tunnel_start (GstRTSPWatch * watch, gpointer user_data)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (user_data);
  GstRTSPClientPrivate *priv = client->priv;

  GST_INFO ("client %p: tunnel start (connection %p)", client,
      priv->connection);

  if (!remember_tunnel (client))
    goto tunnel_error;

  return GST_RTSP_STS_OK;

  /* ERRORS */
tunnel_error:
  {
    GST_ERROR ("client %p: error starting tunnel", client);
    return GST_RTSP_STS_SERVICE_UNAVAILABLE;
  }
}

static GstRTSPResult
tunnel_lost (GstRTSPWatch * watch, gpointer user_data)
{
  GstRTSPClient *client = GST_RTSP_CLIENT (user_data);
  GstRTSPClientPrivate *priv = client->priv;

  GST_WARNING ("client %p: tunnel lost (connection %p)", client,
      priv->connection);

  /* ignore error, it'll only be a problem when the client does a POST again */
  remember_tunnel (client);

  return GST_RTSP_OK;
}

static GstRTSPResult
tunnel_complete (GstRTSPWatch * watch, gpointer user_data)
{
  const gchar *tunnelid;
  GstRTSPClient *client = GST_RTSP_CLIENT (user_data);
  GstRTSPClientPrivate *priv = client->priv;
  GstRTSPClient *oclient;
  GstRTSPClientPrivate *opriv;

  GST_INFO ("client %p: tunnel complete", client);

  /* find previous tunnel */
  tunnelid = gst_rtsp_connection_get_tunnelid (priv->connection);
  if (tunnelid == NULL)
    goto no_tunnelid;

  g_mutex_lock (&tunnels_lock);
  if (!(oclient = g_hash_table_lookup (tunnels, tunnelid)))
    goto no_tunnel;

  /* remove the old client from the table. ref before because removing it will
   * remove the ref to it. */
  g_object_ref (oclient);
  g_hash_table_remove (tunnels, tunnelid);

  opriv = oclient->priv;

  if (opriv->watch == NULL)
    goto tunnel_closed;
  g_mutex_unlock (&tunnels_lock);

  GST_INFO ("client %p: found tunnel %p (old %p, new %p)", client, oclient,
      opriv->connection, priv->connection);

  /* merge the tunnels into the first client */
  gst_rtsp_connection_do_tunnel (opriv->connection, priv->connection);
  gst_rtsp_watch_reset (opriv->watch);
  g_object_unref (oclient);

  return GST_RTSP_OK;

  /* ERRORS */
no_tunnelid:
  {
    GST_ERROR ("client %p: no tunnelid provided", client);
    return GST_RTSP_ERROR;
  }
no_tunnel:
  {
    g_mutex_unlock (&tunnels_lock);
    GST_ERROR ("client %p: tunnel session %s not found", client, tunnelid);
    return GST_RTSP_ERROR;
  }
tunnel_closed:
  {
    g_mutex_unlock (&tunnels_lock);
    GST_ERROR ("client %p: tunnel session %s was closed", client, tunnelid);
    g_object_unref (oclient);
    return GST_RTSP_ERROR;
  }
}

static GstRTSPWatchFuncs watch_funcs = {
  message_received,
  message_sent,
  closed,
  error,
  tunnel_start,
  tunnel_complete,
  error_full,
  tunnel_lost
};

static void
client_watch_notify (GstRTSPClient * client)
{
  GstRTSPClientPrivate *priv = client->priv;

  GST_INFO ("client %p: watch destroyed", client);
  priv->watch = NULL;
  g_signal_emit (client, gst_rtsp_client_signals[SIGNAL_CLOSED], 0, NULL);
  g_object_unref (client);
}

/**
 * gst_rtsp_client_attach:
 * @client: a #GstRTSPClient
 * @context: (allow-none): a #GMainContext
 *
 * Attaches @client to @context. When the mainloop for @context is run, the
 * client will be dispatched. When @context is %NULL, the default context will be
 * used).
 *
 * This function should be called when the client properties and urls are fully
 * configured and the client is ready to start.
 *
 * Returns: the ID (greater than 0) for the source within the GMainContext.
 */
guint
gst_rtsp_client_attach (GstRTSPClient * client, GMainContext * context)
{
  GstRTSPClientPrivate *priv;
  guint res;

  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), 0);
  priv = client->priv;
  g_return_val_if_fail (priv->connection != NULL, 0);
  g_return_val_if_fail (priv->watch == NULL, 0);

  /* create watch for the connection and attach */
  priv->watch = gst_rtsp_watch_new (priv->connection, &watch_funcs,
      g_object_ref (client), (GDestroyNotify) client_watch_notify);
  gst_rtsp_client_set_send_func (client, do_send_message, priv->watch,
      (GDestroyNotify) gst_rtsp_watch_unref);

  /* FIXME make this configurable. We don't want to do this yet because it will
   * be superceeded by a cache object later */
  gst_rtsp_watch_set_send_backlog (priv->watch, 0, 100);

  GST_INFO ("attaching to context %p", context);
  res = gst_rtsp_watch_attach (priv->watch, context);

  return res;
}

/**
 * gst_rtsp_client_session_filter:
 * @client: a #GstRTSPClient
 * @func: (scope call) (allow-none): a callback
 * @user_data: user data passed to @func
 *
 * Call @func for each session managed by @client. The result value of @func
 * determines what happens to the session. @func will be called with @client
 * locked so no further actions on @client can be performed from @func.
 *
 * If @func returns #GST_RTSP_FILTER_REMOVE, the session will be removed from
 * @client.
 *
 * If @func returns #GST_RTSP_FILTER_KEEP, the session will remain in @client.
 *
 * If @func returns #GST_RTSP_FILTER_REF, the session will remain in @client but
 * will also be added with an additional ref to the result #GList of this
 * function..
 *
 * When @func is %NULL, #GST_RTSP_FILTER_REF will be assumed for each session.
 *
 * Returns: (element-type GstRTSPSession) (transfer full): a #GList with all
 * sessions for which @func returned #GST_RTSP_FILTER_REF. After usage, each
 * element in the #GList should be unreffed before the list is freed.
 */
GList *
gst_rtsp_client_session_filter (GstRTSPClient * client,
    GstRTSPClientSessionFilterFunc func, gpointer user_data)
{
  GstRTSPClientPrivate *priv;
  GList *result, *walk, *next;

  g_return_val_if_fail (GST_IS_RTSP_CLIENT (client), NULL);

  priv = client->priv;

  result = NULL;

  g_mutex_lock (&priv->lock);
  for (walk = priv->sessions; walk; walk = next) {
    GstRTSPSession *sess = walk->data;
    GstRTSPFilterResult res;

    next = g_list_next (walk);

    if (func)
      res = func (client, sess, user_data);
    else
      res = GST_RTSP_FILTER_REF;

    switch (res) {
      case GST_RTSP_FILTER_REMOVE:
        /* stop watching the session and pretent it went away */
        client_cleanup_session (client, sess);
        break;
      case GST_RTSP_FILTER_REF:
        result = g_list_prepend (result, g_object_ref (sess));
        break;
      case GST_RTSP_FILTER_KEEP:
      default:
        break;
    }
  }
  g_mutex_unlock (&priv->lock);

  return result;
}
