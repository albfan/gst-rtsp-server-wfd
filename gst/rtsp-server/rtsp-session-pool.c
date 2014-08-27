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
 * SECTION:rtsp-session-pool
 * @short_description: An object for managing sessions
 * @see_also: #GstRTSPSession
 *
 * The #GstRTSPSessionPool object manages a list of #GstRTSPSession objects.
 *
 * The maximum number of sessions can be configured with
 * gst_rtsp_session_pool_set_max_sessions(). The current number of sessions can
 * be retrieved with gst_rtsp_session_pool_get_n_sessions().
 *
 * Use gst_rtsp_session_pool_create() to create a new #GstRTSPSession object.
 * The session object can be found again with its id and
 * gst_rtsp_session_pool_find().
 *
 * All sessions can be iterated with gst_rtsp_session_pool_filter().
 *
 * Run gst_rtsp_session_pool_cleanup() periodically to remove timed out sessions
 * or use gst_rtsp_session_pool_create_watch() to be notified when session
 * cleanup should be performed.
 *
 * Last reviewed on 2013-07-11 (1.0.0)
 */

#include "rtsp-session-pool.h"

#define GST_RTSP_SESSION_POOL_GET_PRIVATE(obj)  \
         (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_RTSP_SESSION_POOL, GstRTSPSessionPoolPrivate))

struct _GstRTSPSessionPoolPrivate
{
  GMutex lock;                  /* protects everything in this struct */
  guint max_sessions;
  GHashTable *sessions;
};

#define DEFAULT_MAX_SESSIONS 0

enum
{
  PROP_0,
  PROP_MAX_SESSIONS,
  PROP_LAST
};

static const gchar session_id_charset[] =
    { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
  'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 'A', 'B', 'C', 'D',
  'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
  'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '0', '1', '2', '3', '4', '5', '6', '7',
  '8', '9', '$', '-', '_', '.', '+'
};

GST_DEBUG_CATEGORY_STATIC (rtsp_session_debug);
#define GST_CAT_DEFAULT rtsp_session_debug

static void gst_rtsp_session_pool_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec);
static void gst_rtsp_session_pool_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec);
static void gst_rtsp_session_pool_finalize (GObject * object);

static gchar *create_session_id (GstRTSPSessionPool * pool);
static GstRTSPSession *create_session (GstRTSPSessionPool * pool,
    const gchar * id);

G_DEFINE_TYPE (GstRTSPSessionPool, gst_rtsp_session_pool, G_TYPE_OBJECT);

static void
gst_rtsp_session_pool_class_init (GstRTSPSessionPoolClass * klass)
{
  GObjectClass *gobject_class;

  g_type_class_add_private (klass, sizeof (GstRTSPSessionPoolPrivate));

  gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = gst_rtsp_session_pool_get_property;
  gobject_class->set_property = gst_rtsp_session_pool_set_property;
  gobject_class->finalize = gst_rtsp_session_pool_finalize;

  g_object_class_install_property (gobject_class, PROP_MAX_SESSIONS,
      g_param_spec_uint ("max-sessions", "Max Sessions",
          "the maximum amount of sessions (0 = unlimited)",
          0, G_MAXUINT, DEFAULT_MAX_SESSIONS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  klass->create_session_id = create_session_id;
  klass->create_session = create_session;

  GST_DEBUG_CATEGORY_INIT (rtsp_session_debug, "rtspsessionpool", 0,
      "GstRTSPSessionPool");
}

static void
gst_rtsp_session_pool_init (GstRTSPSessionPool * pool)
{
  GstRTSPSessionPoolPrivate *priv = GST_RTSP_SESSION_POOL_GET_PRIVATE (pool);

  pool->priv = priv;

  g_mutex_init (&priv->lock);
  priv->sessions = g_hash_table_new_full (g_str_hash, g_str_equal,
      NULL, g_object_unref);
  priv->max_sessions = DEFAULT_MAX_SESSIONS;
}

static void
gst_rtsp_session_pool_finalize (GObject * object)
{
  GstRTSPSessionPool *pool = GST_RTSP_SESSION_POOL (object);
  GstRTSPSessionPoolPrivate *priv = pool->priv;

  g_mutex_clear (&priv->lock);
  g_hash_table_unref (priv->sessions);

  G_OBJECT_CLASS (gst_rtsp_session_pool_parent_class)->finalize (object);
}

static void
gst_rtsp_session_pool_get_property (GObject * object, guint propid,
    GValue * value, GParamSpec * pspec)
{
  GstRTSPSessionPool *pool = GST_RTSP_SESSION_POOL (object);

  switch (propid) {
    case PROP_MAX_SESSIONS:
      g_value_set_uint (value, gst_rtsp_session_pool_get_max_sessions (pool));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
      break;
  }
}

static void
gst_rtsp_session_pool_set_property (GObject * object, guint propid,
    const GValue * value, GParamSpec * pspec)
{
  GstRTSPSessionPool *pool = GST_RTSP_SESSION_POOL (object);

  switch (propid) {
    case PROP_MAX_SESSIONS:
      gst_rtsp_session_pool_set_max_sessions (pool, g_value_get_uint (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, propid, pspec);
      break;
  }
}

/**
 * gst_rtsp_session_pool_new:
 *
 * Create a new #GstRTSPSessionPool instance.
 *
 * Returns: A new #GstRTSPSessionPool. g_object_unref() after usage.
 */
GstRTSPSessionPool *
gst_rtsp_session_pool_new (void)
{
  GstRTSPSessionPool *result;

  result = g_object_new (GST_TYPE_RTSP_SESSION_POOL, NULL);

  return result;
}

/**
 * gst_rtsp_session_pool_set_max_sessions:
 * @pool: a #GstRTSPSessionPool
 * @max: the maximum number of sessions
 *
 * Configure the maximum allowed number of sessions in @pool to @max.
 * A value of 0 means an unlimited amount of sessions.
 */
void
gst_rtsp_session_pool_set_max_sessions (GstRTSPSessionPool * pool, guint max)
{
  GstRTSPSessionPoolPrivate *priv;

  g_return_if_fail (GST_IS_RTSP_SESSION_POOL (pool));

  priv = pool->priv;

  g_mutex_lock (&priv->lock);
  priv->max_sessions = max;
  g_mutex_unlock (&priv->lock);
}

/**
 * gst_rtsp_session_pool_get_max_sessions:
 * @pool: a #GstRTSPSessionPool
 *
 * Get the maximum allowed number of sessions in @pool. 0 means an unlimited
 * amount of sessions.
 *
 * Returns: the maximum allowed number of sessions.
 */
guint
gst_rtsp_session_pool_get_max_sessions (GstRTSPSessionPool * pool)
{
  GstRTSPSessionPoolPrivate *priv;
  guint result;

  g_return_val_if_fail (GST_IS_RTSP_SESSION_POOL (pool), 0);

  priv = pool->priv;

  g_mutex_lock (&priv->lock);
  result = priv->max_sessions;
  g_mutex_unlock (&priv->lock);

  return result;
}

/**
 * gst_rtsp_session_pool_get_n_sessions:
 * @pool: a #GstRTSPSessionPool
 *
 * Get the amount of active sessions in @pool.
 *
 * Returns: the amount of active sessions in @pool.
 */
guint
gst_rtsp_session_pool_get_n_sessions (GstRTSPSessionPool * pool)
{
  GstRTSPSessionPoolPrivate *priv;
  guint result;

  g_return_val_if_fail (GST_IS_RTSP_SESSION_POOL (pool), 0);

  priv = pool->priv;

  g_mutex_lock (&priv->lock);
  result = g_hash_table_size (priv->sessions);
  g_mutex_unlock (&priv->lock);

  return result;
}

/**
 * gst_rtsp_session_pool_find:
 * @pool: the pool to search
 * @sessionid: the session id
 *
 * Find the session with @sessionid in @pool. The access time of the session
 * will be updated with gst_rtsp_session_touch().
 *
 * Returns: (transfer full): the #GstRTSPSession with @sessionid or %NULL when the session did
 * not exist. g_object_unref() after usage.
 */
GstRTSPSession *
gst_rtsp_session_pool_find (GstRTSPSessionPool * pool, const gchar * sessionid)
{
  GstRTSPSessionPoolPrivate *priv;
  GstRTSPSession *result;

  g_return_val_if_fail (GST_IS_RTSP_SESSION_POOL (pool), NULL);
  g_return_val_if_fail (sessionid != NULL, NULL);

  priv = pool->priv;

  g_mutex_lock (&priv->lock);
  result = g_hash_table_lookup (priv->sessions, sessionid);
  if (result) {
    g_object_ref (result);
    gst_rtsp_session_touch (result);
  }
  g_mutex_unlock (&priv->lock);

  return result;
}

static gchar *
create_session_id (GstRTSPSessionPool * pool)
{
  gchar id[17];
  gint i;

  for (i = 0; i < 16; i++) {
    id[i] =
        session_id_charset[g_random_int_range (0,
            G_N_ELEMENTS (session_id_charset))];
  }
  id[16] = 0;

  return g_uri_escape_string (id, NULL, FALSE);
}

static GstRTSPSession *
create_session (GstRTSPSessionPool * pool, const gchar * id)
{
  return gst_rtsp_session_new (id);
}

/**
 * gst_rtsp_session_pool_create:
 * @pool: a #GstRTSPSessionPool
 *
 * Create a new #GstRTSPSession object in @pool.
 *
 * Returns: (transfer none): a new #GstRTSPSession.
 */
GstRTSPSession *
gst_rtsp_session_pool_create (GstRTSPSessionPool * pool)
{
  GstRTSPSessionPoolPrivate *priv;
  GstRTSPSession *result = NULL;
  GstRTSPSessionPoolClass *klass;
  gchar *id = NULL;
  guint retry;

  g_return_val_if_fail (GST_IS_RTSP_SESSION_POOL (pool), NULL);

  priv = pool->priv;

  klass = GST_RTSP_SESSION_POOL_GET_CLASS (pool);

  retry = 0;
  do {
    /* start by creating a new random session id, we assume that this is random
     * enough to not cause a collision, which we will check later  */
    if (klass->create_session_id)
      id = klass->create_session_id (pool);
    else
      goto no_function;

    if (id == NULL)
      goto no_session;

    g_mutex_lock (&priv->lock);
    /* check session limit */
    if (priv->max_sessions > 0) {
      if (g_hash_table_size (priv->sessions) >= priv->max_sessions)
        goto too_many_sessions;
    }
    /* check if the sessionid existed */
    result = g_hash_table_lookup (priv->sessions, id);
    if (result) {
      /* found, retry with a different session id */
      result = NULL;
      retry++;
      if (retry > 100)
        goto collision;
    } else {
      /* not found, create session and insert it in the pool */
      if (klass->create_session)
        result = create_session (pool, id);
      if (result == NULL)
        goto too_many_sessions;
      /* take additional ref for the pool */
      g_object_ref (result);
      g_hash_table_insert (priv->sessions,
          (gchar *) gst_rtsp_session_get_sessionid (result), result);
    }
    g_mutex_unlock (&priv->lock);

    g_free (id);
  } while (result == NULL);

  return result;

  /* ERRORS */
no_function:
  {
    GST_WARNING ("no create_session_id vmethod in GstRTSPSessionPool %p", pool);
    return NULL;
  }
no_session:
  {
    GST_WARNING ("can't create session id with GstRTSPSessionPool %p", pool);
    return NULL;
  }
collision:
  {
    GST_WARNING ("can't find unique sessionid for GstRTSPSessionPool %p", pool);
    g_mutex_unlock (&priv->lock);
    g_free (id);
    return NULL;
  }
too_many_sessions:
  {
    GST_WARNING ("session pool reached max sessions of %d", priv->max_sessions);
    g_mutex_unlock (&priv->lock);
    g_free (id);
    return NULL;
  }
}

/**
 * gst_rtsp_session_pool_remove:
 * @pool: a #GstRTSPSessionPool
 * @sess: a #GstRTSPSession
 *
 * Remove @sess from @pool, releasing the ref that the pool has on @sess.
 *
 * Returns: %TRUE if the session was found and removed.
 */
gboolean
gst_rtsp_session_pool_remove (GstRTSPSessionPool * pool, GstRTSPSession * sess)
{
  GstRTSPSessionPoolPrivate *priv;
  gboolean found;

  g_return_val_if_fail (GST_IS_RTSP_SESSION_POOL (pool), FALSE);
  g_return_val_if_fail (GST_IS_RTSP_SESSION (sess), FALSE);

  priv = pool->priv;

  g_mutex_lock (&priv->lock);
  found =
      g_hash_table_remove (priv->sessions,
      gst_rtsp_session_get_sessionid (sess));
  g_mutex_unlock (&priv->lock);

  return found;
}

static gboolean
cleanup_func (gchar * sessionid, GstRTSPSession * sess, GTimeVal * now)
{
  return gst_rtsp_session_is_expired (sess, now);
}

/**
 * gst_rtsp_session_pool_cleanup:
 * @pool: a #GstRTSPSessionPool
 *
 * Inspect all the sessions in @pool and remove the sessions that are inactive
 * for more than their timeout.
 *
 * Returns: the amount of sessions that got removed.
 */
guint
gst_rtsp_session_pool_cleanup (GstRTSPSessionPool * pool)
{
  GstRTSPSessionPoolPrivate *priv;
  guint result;
  GTimeVal now;

  g_return_val_if_fail (GST_IS_RTSP_SESSION_POOL (pool), 0);

  priv = pool->priv;

  g_get_current_time (&now);

  g_mutex_lock (&priv->lock);
  result =
      g_hash_table_foreach_remove (priv->sessions, (GHRFunc) cleanup_func,
      &now);
  g_mutex_unlock (&priv->lock);

  return result;
}

typedef struct
{
  GstRTSPSessionPool *pool;
  GstRTSPSessionPoolFilterFunc func;
  gpointer user_data;
  GList *list;
} FilterData;

static gboolean
filter_func (gchar * sessionid, GstRTSPSession * sess, FilterData * data)
{
  GstRTSPFilterResult res;

  if (data->func)
    res = data->func (data->pool, sess, data->user_data);
  else
    res = GST_RTSP_FILTER_REF;

  switch (res) {
    case GST_RTSP_FILTER_REMOVE:
      return TRUE;
    case GST_RTSP_FILTER_REF:
      /* keep ref */
      data->list = g_list_prepend (data->list, g_object_ref (sess));
      /* fallthrough */
    default:
    case GST_RTSP_FILTER_KEEP:
      return FALSE;
  }
}

/**
 * gst_rtsp_session_pool_filter:
 * @pool: a #GstRTSPSessionPool
 * @func: (scope call) (allow-none): a callback
 * @user_data: user data passed to @func
 *
 * Call @func for each session in @pool. The result value of @func determines
 * what happens to the session. @func will be called with the session pool
 * locked so no further actions on @pool can be performed from @func.
 *
 * If @func returns #GST_RTSP_FILTER_REMOVE, the session will be removed from
 * @pool.
 *
 * If @func returns #GST_RTSP_FILTER_KEEP, the session will remain in @pool.
 *
 * If @func returns #GST_RTSP_FILTER_REF, the session will remain in @pool but
 * will also be added with an additional ref to the result GList of this
 * function..
 *
 * When @func is %NULL, #GST_RTSP_FILTER_REF will be assumed for all sessions.
 *
 * Returns: (element-type GstRTSPSession) (transfer full): a GList with all
 * sessions for which @func returned #GST_RTSP_FILTER_REF. After usage, each
 * element in the GList should be unreffed before the list is freed.
 */
GList *
gst_rtsp_session_pool_filter (GstRTSPSessionPool * pool,
    GstRTSPSessionPoolFilterFunc func, gpointer user_data)
{
  GstRTSPSessionPoolPrivate *priv;
  FilterData data;

  g_return_val_if_fail (GST_IS_RTSP_SESSION_POOL (pool), NULL);

  priv = pool->priv;

  data.pool = pool;
  data.func = func;
  data.user_data = user_data;
  data.list = NULL;

  g_mutex_lock (&priv->lock);
  g_hash_table_foreach_remove (priv->sessions, (GHRFunc) filter_func, &data);
  g_mutex_unlock (&priv->lock);

  return data.list;
}

typedef struct
{
  GSource source;
  GstRTSPSessionPool *pool;
  gint timeout;
} GstPoolSource;

static void
collect_timeout (gchar * sessionid, GstRTSPSession * sess, GstPoolSource * psrc)
{
  gint timeout;
  GTimeVal now;

  g_get_current_time (&now);

  timeout = gst_rtsp_session_next_timeout (sess, &now);
  GST_INFO ("%p: next timeout: %d", sess, timeout);
  if (psrc->timeout == -1 || timeout < psrc->timeout)
    psrc->timeout = timeout;
}

static gboolean
gst_pool_source_prepare (GSource * source, gint * timeout)
{
  GstRTSPSessionPoolPrivate *priv;
  GstPoolSource *psrc;
  gboolean result;

  psrc = (GstPoolSource *) source;
  psrc->timeout = -1;
  priv = psrc->pool->priv;

  g_mutex_lock (&priv->lock);
  g_hash_table_foreach (priv->sessions, (GHFunc) collect_timeout, psrc);
  g_mutex_unlock (&priv->lock);

  if (timeout)
    *timeout = psrc->timeout;

  result = psrc->timeout == 0;

  GST_INFO ("prepare %d, %d", psrc->timeout, result);

  return result;
}

static gboolean
gst_pool_source_check (GSource * source)
{
  GST_INFO ("check");

  return gst_pool_source_prepare (source, NULL);
}

static gboolean
gst_pool_source_dispatch (GSource * source, GSourceFunc callback,
    gpointer user_data)
{
  gboolean res;
  GstPoolSource *psrc = (GstPoolSource *) source;
  GstRTSPSessionPoolFunc func = (GstRTSPSessionPoolFunc) callback;

  GST_INFO ("dispatch");

  if (func)
    res = func (psrc->pool, user_data);
  else
    res = FALSE;

  return res;
}

static void
gst_pool_source_finalize (GSource * source)
{
  GstPoolSource *psrc = (GstPoolSource *) source;

  GST_INFO ("finalize %p", psrc);

  g_object_unref (psrc->pool);
  psrc->pool = NULL;
}

static GSourceFuncs gst_pool_source_funcs = {
  gst_pool_source_prepare,
  gst_pool_source_check,
  gst_pool_source_dispatch,
  gst_pool_source_finalize
};

/**
 * gst_rtsp_session_pool_create_watch:
 * @pool: a #GstRTSPSessionPool
 *
 * A GSource that will be dispatched when the session should be cleaned up.
 */
GSource *
gst_rtsp_session_pool_create_watch (GstRTSPSessionPool * pool)
{
  GstPoolSource *source;

  g_return_val_if_fail (GST_IS_RTSP_SESSION_POOL (pool), NULL);

  source = (GstPoolSource *) g_source_new (&gst_pool_source_funcs,
      sizeof (GstPoolSource));
  source->pool = g_object_ref (pool);

  return (GSource *) source;
}
