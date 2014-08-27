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
 * SECTION:rtsp-media-factory
 * @short_description: A factory for media pipelines
 * @see_also: #GstRTSPMountPoints, #GstRTSPMedia
 *
 * The #GstRTSPMediaFactoryWFD is responsible for creating or recycling
 * #GstRTSPMedia objects based on the passed URL.
 *
 * The default implementation of the object can create #GstRTSPMedia objects
 * containing a pipeline created from a launch description set with
 * gst_rtsp_media_factory_wfd_set_launch().
 *
 * Media from a factory can be shared by setting the shared flag with
 * gst_rtsp_media_factory_wfd_set_shared(). When a factory is shared,
 * gst_rtsp_media_factory_wfd_construct() will return the same #GstRTSPMedia when
 * the url matches.
 *
 * Last reviewed on 2013-07-11 (1.0.0)
 */

#include "rtsp-media-factory-wfd.h"

#define GST_RTSP_MEDIA_FACTORY_WFD_GET_PRIVATE(obj)  \
       (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_RTSP_MEDIA_FACTORY_WFD, GstRTSPMediaFactoryWFDPrivate))

#define GST_RTSP_MEDIA_FACTORY_WFD_GET_LOCK(f)       (&(GST_RTSP_MEDIA_FACTORY_WFD_CAST(f)->priv->lock))
#define GST_RTSP_MEDIA_FACTORY_WFD_LOCK(f)           (g_mutex_lock(GST_RTSP_MEDIA_FACTORY_WFD_GET_LOCK(f)))
#define GST_RTSP_MEDIA_FACTORY_WFD_UNLOCK(f)         (g_mutex_unlock(GST_RTSP_MEDIA_FACTORY_WFD_GET_LOCK(f)))

struct _GstRTSPMediaFactoryWFDPrivate
{
  GMutex lock;                  /* protects everything but medias */
  GstRTSPPermissions *permissions;
  gchar *launch;
  gboolean shared;
  GstRTSPSuspendMode suspend_mode;
  gboolean eos_shutdown;
  GstRTSPLowerTrans protocols;
  guint buffer_size;
  GstRTSPAddressPool *pool;

  GMutex medias_lock;
  GHashTable *medias;           /* protected by medias_lock */
};

#define DEFAULT_LAUNCH          NULL
#define DEFAULT_SHARED          FALSE
#define DEFAULT_SUSPEND_MODE    GST_RTSP_SUSPEND_MODE_NONE
#define DEFAULT_EOS_SHUTDOWN    FALSE
#define DEFAULT_PROTOCOLS       GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_UDP_MCAST | \
                                        GST_RTSP_LOWER_TRANS_TCP
#define DEFAULT_BUFFER_SIZE     0x80000

enum
{
  PROP_0,
  PROP_LAUNCH,
  PROP_SHARED,
  PROP_SUSPEND_MODE,
  PROP_EOS_SHUTDOWN,
  PROP_PROTOCOLS,
  PROP_BUFFER_SIZE,
  PROP_LAST
};

enum
{
  SIGNAL_MEDIA_CONSTRUCTED,
  SIGNAL_MEDIA_CONFIGURE,
  SIGNAL_LAST
};

GST_DEBUG_CATEGORY_STATIC (rtsp_media_wfd_debug);
#define GST_CAT_DEFAULT rtsp_media_wfd_debug

static void gst_rtsp_media_factory_wfd_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec);
static void gst_rtsp_media_factory_wfd_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec);

static void gst_rtsp_media_factory_wfd_finalize (GObject * obj);

G_DEFINE_TYPE (GstRTSPMediaFactoryWFD, gst_rtsp_media_factory_wfd, GST_TYPE_RTSP_MEDIA_FACTORY);

static void
gst_rtsp_media_factory_wfd_class_init (GstRTSPMediaFactoryWFDClass * klass)
{
  GObjectClass *gobject_class;

  g_type_class_add_private (klass, sizeof (GstRTSPMediaFactoryWFDPrivate));

  gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = gst_rtsp_media_factory_wfd_get_property;
  gobject_class->set_property = gst_rtsp_media_factory_wfd_set_property;
  gobject_class->finalize = gst_rtsp_media_factory_wfd_finalize;


  //klass->gen_key = default_gen_key;
  //klass->create_element = default_create_element;
  //klass->construct = default_construct;
  //klass->configure = default_configure;
  //klass->create_pipeline = default_create_pipeline;

  GST_DEBUG_CATEGORY_INIT (rtsp_media_wfd_debug, "rtspmediafactorywfd", 0,
      "GstRTSPMediaFactoryWFD");
}

static void
gst_rtsp_media_factory_wfd_init (GstRTSPMediaFactoryWFD * factory)
{
  GstRTSPMediaFactoryWFDPrivate *priv =
      GST_RTSP_MEDIA_FACTORY_WFD_GET_PRIVATE (factory);
  factory->priv = priv;

  priv->launch = g_strdup (DEFAULT_LAUNCH);
  priv->shared = DEFAULT_SHARED;
  priv->suspend_mode = DEFAULT_SUSPEND_MODE;
  priv->eos_shutdown = DEFAULT_EOS_SHUTDOWN;
  priv->protocols = DEFAULT_PROTOCOLS;
  priv->buffer_size = DEFAULT_BUFFER_SIZE;

  g_mutex_init (&priv->lock);
  g_mutex_init (&priv->medias_lock);
  priv->medias = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, g_object_unref);
}

static void
gst_rtsp_media_factory_wfd_finalize (GObject * obj)
{
  GstRTSPMediaFactoryWFD *factory = GST_RTSP_MEDIA_FACTORY_WFD (obj);
  GstRTSPMediaFactoryWFDPrivate *priv = factory->priv;

  if (priv->permissions)
    gst_rtsp_permissions_unref (priv->permissions);
  g_hash_table_unref (priv->medias);
  g_mutex_clear (&priv->medias_lock);
  g_free (priv->launch);
  g_mutex_clear (&priv->lock);
  if (priv->pool)
    g_object_unref (priv->pool);

  G_OBJECT_CLASS (gst_rtsp_media_factory_wfd_parent_class)->finalize (obj);
}

GstRTSPMediaFactoryWFD *
gst_rtsp_media_factory_wfd_new (void)
{
  GstRTSPMediaFactoryWFD *result;

  result = g_object_new (GST_TYPE_RTSP_MEDIA_FACTORY_WFD, NULL);

  return result;
}

static void
gst_rtsp_media_factory_wfd_get_property (GObject * object,
             guint propid, GValue * value, GParamSpec * pspec)
{
  //GstRTSPMediaFactoryWFD *factory = GST_RTSP_MEDIA_FACTORY_WFD (object);

  switch (propid) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

static void
gst_rtsp_media_factory_wfd_set_property (GObject * object,
             guint propid, const GValue * value, GParamSpec * pspec)
{
  //GstRTSPMediaFactoryWFD *factory = GST_RTSP_MEDIA_FACTORY_WFD (object);

  switch (propid) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
  }
}

/**
 * gst_rtsp_media_factory_wfd_create_element:
 * @factory: a #GstRTSPMediaFactoryWFD
 * @url: the url used
 *
 * Construct and return a #GstElement that is a #GstBin containing
 * the elements to use for streaming the media.
 *
 * The bin should contain payloaders pay\%d for each stream. The default
 * implementation of this function returns the bin created from the
 * launch parameter.
 *
 * Returns: (transfer floating) a new #GstElement.
 */
GstElement *
gst_rtsp_media_factory_wfd_create_element (GstRTSPMediaFactoryWFD * factory,
    const GstRTSPUrl * url)
{
  //GstRTSPMediaFactoryWFDClass *klass;
  GstElement *result;

  g_return_val_if_fail (GST_IS_RTSP_MEDIA_FACTORY_WFD (factory), NULL);
  g_return_val_if_fail (url != NULL, NULL);

  //klass = GST_RTSP_MEDIA_FACTORY_WFD_GET_CLASS (factory);

#if 0
  if (klass->create_element)
    result = klass->create_element (factory, url);
  else
    result = NULL;
#else
#endif

  return result;
}
