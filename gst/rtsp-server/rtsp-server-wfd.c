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
 * SECTION:rtsp-server
 * @short_description: The main server object
 * @see_also: #GstRTSPClient, #GstRTSPThreadPool
 *
 * The server object is the object listening for connections on a port and
 * creating #GstRTSPClient objects to handle those connections.
 *
 * The server will listen on the address set with gst_rtsp_server_set_address()
 * and the port or service configured with gst_rtsp_server_set_service().
 * Use gst_rtsp_server_set_backlog() to configure the amount of pending requests
 * that the server will keep. By default the server listens on the current
 * network (0.0.0.0) and port 8554.
 *
 * The server will require an SSL connection when a TLS certificate has been
 * set in the auth object with gst_rtsp_auth_set_tls_certificate().
 *
 * To start the server, use gst_rtsp_server_attach() to attach it to a
 * #GMainContext. For more control, gst_rtsp_server_create_source() and
 * gst_rtsp_server_create_socket() can be used to get a #GSource and #GSocket
 * respectively.
 *
 * gst_rtsp_server_transfer_connection() can be used to transfer an existing
 * socket to the RTSP server, for example from an HTTP server.
 *
 * Once the server socket is attached to a mainloop, it will start accepting
 * connections. When a new connection is received, a new #GstRTSPClient object
 * is created to handle the connection. The new client will be configured with
 * the server #GstRTSPAuth, #GstRTSPMountPoints, #GstRTSPSessionPool and
 * #GstRTSPThreadPool.
 *
 * The server uses the configured #GstRTSPThreadPool object to handle the
 * remainder of the communication with this client.
 *
 * Last reviewed on 2013-07-11 (1.0.0)
 */
#include <stdlib.h>
#include <string.h>

#include "rtsp-server-wfd.h"
#include "rtsp-client-wfd.h"

#define GST_RTSP_WFD_SERVER_GET_PRIVATE(obj)  \
       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_RTSP_WFD_SERVER, GstRTSPWFDServerPrivate))

#define GST_RTSP_WFD_SERVER_GET_LOCK(server)  (&(GST_RTSP_WFD_SERVER_CAST(server)->priv->lock))
#define GST_RTSP_WFD_SERVER_LOCK(server)      (g_mutex_lock(GST_RTSP_WFD_SERVER_GET_LOCK(server)))
#define GST_RTSP_WFD_SERVER_UNLOCK(server)    (g_mutex_unlock(GST_RTSP_WFD_SERVER_GET_LOCK(server)))

struct _GstRTSPWFDServerPrivate
{
  GMutex lock;                  /* protects everything in this struct */

  /* the clients that are connected */
  GList *clients;
};

G_DEFINE_TYPE (GstRTSPWFDServer, gst_rtsp_wfd_server, GST_TYPE_RTSP_SERVER);

GST_DEBUG_CATEGORY_STATIC (rtsp_wfd_server_debug);
#define GST_CAT_DEFAULT rtsp_wfd_server_debug

static void gst_rtsp_wfd_server_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec);
static void gst_rtsp_wfd_server_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec);
static void gst_rtsp_wfd_server_finalize (GObject * object);

static GstRTSPClient *create_client_wfd (GstRTSPServer * server);
static void client_connected_wfd (GstRTSPServer * server, GstRTSPClient *client);

static void
gst_rtsp_wfd_server_class_init (GstRTSPWFDServerClass * klass)
{
  GObjectClass *gobject_class;
  GstRTSPServerClass *rtsp_server_class;

  g_type_class_add_private (klass, sizeof (GstRTSPWFDServerPrivate));

  gobject_class = G_OBJECT_CLASS (klass);
  rtsp_server_class = GST_RTSP_SERVER_CLASS(klass);

  gobject_class->get_property = gst_rtsp_wfd_server_get_property;
  gobject_class->set_property = gst_rtsp_wfd_server_set_property;
  gobject_class->finalize = gst_rtsp_wfd_server_finalize;

  rtsp_server_class->create_client = create_client_wfd;
  rtsp_server_class->client_connected = client_connected_wfd;


  GST_DEBUG_CATEGORY_INIT (rtsp_wfd_server_debug, "rtspwfdserver", 0, "GstRTSPWFDServer");
}

static void
gst_rtsp_wfd_server_init (GstRTSPWFDServer * server)
{
  GstRTSPWFDServerPrivate *priv = GST_RTSP_WFD_SERVER_GET_PRIVATE (server);

  server->priv = priv;
  GST_INFO_OBJECT(server, "New server is initialized");
}

static void
gst_rtsp_wfd_server_finalize (GObject * object)
{
  GstRTSPWFDServer *server = GST_RTSP_WFD_SERVER (object);
  //GstRTSPWFDServerPrivate *priv = server->priv;

  GST_DEBUG_OBJECT (server, "finalize server");

  G_OBJECT_CLASS (gst_rtsp_wfd_server_parent_class)->finalize (object);
}

/**
 * gst_rtsp_server_new:
 *
 * Create a new #GstRTSPWFDServer instance.
 */
GstRTSPWFDServer *
gst_rtsp_wfd_server_new (void)
{
  GstRTSPWFDServer *result;

  result = g_object_new (GST_TYPE_RTSP_WFD_SERVER, NULL);

  return result;
}

static void
gst_rtsp_wfd_server_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec)
{
  //GstRTSPWFDServer *server = GST_RTSP_WFD_SERVER (object);

  switch (propid) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

static void
gst_rtsp_wfd_server_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec)
{
  //GstRTSPWFDServer *server = GST_RTSP_WFD_SERVER (object);

  switch (propid) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

static gboolean
_start_wfd(gpointer data)
{
  GstRTSPWFDClient *client = (GstRTSPWFDClient *)data;

  GST_INFO_OBJECT(client, "WFD client is STARTing");

  gst_rtsp_wfd_client_start_wfd(client);
  return FALSE;
}

static void
client_connected_wfd (GstRTSPServer * server, GstRTSPClient *client)
{
  GST_INFO_OBJECT(server, "Client is connected");

  g_idle_add(_start_wfd, client);
  return;
}

static GstRTSPClient *
create_client_wfd (GstRTSPServer * server)
{
  GstRTSPWFDClient *client;
  GstRTSPThreadPool *thread_pool = NULL;
  GstRTSPSessionPool *session_pool = NULL;
  GstRTSPMountPoints *mount_points = NULL;
  GstRTSPAuth *auth = NULL;

  GST_INFO_OBJECT(server, "New Client is being created");

  /* a new client connected, create a session to handle the client. */
  client = gst_rtsp_wfd_client_new ();

  thread_pool = gst_rtsp_server_get_thread_pool(server);
  session_pool = gst_rtsp_server_get_session_pool(server);
  mount_points = gst_rtsp_server_get_mount_points(server);
  auth = gst_rtsp_server_get_auth(server);

  /* set the session pool that this client should use */
  GST_RTSP_WFD_SERVER_LOCK (server);
  gst_rtsp_client_set_session_pool (GST_RTSP_CLIENT_CAST(client), session_pool);
  /* set the mount points that this client should use */
  gst_rtsp_client_set_mount_points (GST_RTSP_CLIENT_CAST(client), mount_points);
  /* set authentication manager */
  gst_rtsp_client_set_auth (GST_RTSP_CLIENT_CAST(client), auth);
  /* set threadpool */
  gst_rtsp_client_set_thread_pool (GST_RTSP_CLIENT_CAST(client), thread_pool);
  GST_RTSP_WFD_SERVER_UNLOCK (server);

  return GST_RTSP_CLIENT(client);
}

