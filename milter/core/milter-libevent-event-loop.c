/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 *  Copyright (C) 2010, 2011  Nobuyoshi Nakada <nakada@clear-code.com>
 *
 *  This library is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this library.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#  include "../../config.h"
#endif /* HAVE_CONFIG_H */

#include "milter-libevent-event-loop.h"
#include <math.h>
#include <ev.h>

#define MILTER_LIBEVENT_EVENT_LOOP_GET_PRIVATE(obj)              \
  (G_TYPE_INSTANCE_GET_PRIVATE((obj),                   \
                               MILTER_TYPE_LIBEVENT_EVENT_LOOP,  \
                               MilterLibeventEventLoopPrivate))

G_DEFINE_TYPE(MilterLibeventEventLoop, milter_libevent_event_loop, MILTER_TYPE_EVENT_LOOP)

typedef struct _MilterLibeventEventLoopPrivate	MilterLibeventEventLoopPrivate;
struct _MilterLibeventEventLoopPrivate
{
    struct ev_loop *base;
    guint tag;
    GHashTable *callbacks;
};

enum
{
    PROP_0,
    PROP_LAST
};

static void     dispose          (GObject         *object);
static void     constructed      (GObject         *object);

static void     destroy_callback (gpointer         callback);

static void     run              (MilterEventLoop *loop);
static gboolean iterate          (MilterEventLoop *loop,
                                  gboolean         may_block);
static void     quit             (MilterEventLoop *loop);

static guint    watch_io         (MilterEventLoop *loop,
                                  GIOChannel      *channel,
                                  GIOCondition     condition,
                                  GIOFunc          function,
                                  gpointer         data);

static guint    watch_child_full (MilterEventLoop *loop,
                                  gint             priority,
                                  GPid             pid,
                                  GChildWatchFunc  function,
                                  gpointer         data,
                                  GDestroyNotify   notify);

static guint    add_timeout_full (MilterEventLoop *loop,
                                  gint             priority,
                                  gdouble          interval_in_seconds,
                                  GSourceFunc      function,
                                  gpointer         data,
                                  GDestroyNotify   notify);

static guint    add_idle_full    (MilterEventLoop *loop,
                                  gint             priority,
                                  GSourceFunc      function,
                                  gpointer         data,
                                  GDestroyNotify   notify);

static gboolean remove           (MilterEventLoop *loop,
                                  guint            tag);

static void
milter_libevent_event_loop_class_init (MilterLibeventEventLoopClass *klass)
{
    GObjectClass *gobject_class;

    gobject_class = G_OBJECT_CLASS(klass);

    gobject_class->dispose      = dispose;
    gobject_class->constructed  = constructed;

    klass->parent_class.run = run;
    klass->parent_class.iterate = iterate;
    klass->parent_class.quit = quit;
    klass->parent_class.watch_io = watch_io;
    klass->parent_class.watch_child_full = watch_child_full;
    klass->parent_class.add_timeout_full = add_timeout_full;
    klass->parent_class.add_idle_full = add_idle_full;
    klass->parent_class.remove = remove;

    g_type_class_add_private(gobject_class, sizeof(MilterLibeventEventLoopPrivate));
}

static void
milter_libevent_event_loop_init (MilterLibeventEventLoop *loop)
{
    MilterLibeventEventLoopPrivate *priv;

    priv = MILTER_LIBEVENT_EVENT_LOOP_GET_PRIVATE(loop);
    priv->base = NULL;
    priv->tag = 0;
    priv->callbacks = NULL;
}

static void
dispose (GObject *object)
{
    MilterLibeventEventLoopPrivate *priv;

    priv = MILTER_LIBEVENT_EVENT_LOOP_GET_PRIVATE(object);

    if (priv->callbacks) {
        g_hash_table_destroy(priv->callbacks);
        priv->callbacks = NULL;
    }
    if (priv->base) {
        ev_loop_destroy(priv->base);
        priv->base = NULL;
    }

    G_OBJECT_CLASS(milter_libevent_event_loop_parent_class)->dispose(object);
}

static void
constructed (GObject *object)
{
    MilterLibeventEventLoop *loop;
    MilterLibeventEventLoopPrivate *priv;

    loop = MILTER_LIBEVENT_EVENT_LOOP(object);
    priv = MILTER_LIBEVENT_EVENT_LOOP_GET_PRIVATE(loop);
    priv->base = ev_default_loop(0);
    priv->callbacks = g_hash_table_new_full(g_int_hash, g_int_equal,
                                            NULL, destroy_callback);
}

MilterEventLoop *
milter_libevent_event_loop_new (void)
{
    return g_object_new(MILTER_TYPE_LIBEVENT_EVENT_LOOP,
                        NULL);
}

struct callback_funcs {
    void (*stop)(struct ev_loop *, void *);
};

struct callback_header {
    MilterLibeventEventLoop *loop;
    const struct callback_funcs *funcs;
};

static guint
add_callback (MilterLibeventEventLoopPrivate *priv, gpointer data)
{
    guint tag = ++priv->tag;
    g_hash_table_insert(priv->callbacks, (gpointer)(gsize)tag, data);
    return tag;
}

static gpointer
alloc_callback (MilterEventLoop *loop,
                const struct callback_funcs *funcs,
                gsize size)
{
    struct callback_header *header = g_malloc0(size + sizeof(*header));
    header->loop = (MilterLibeventEventLoop *)loop;
    header->funcs = funcs;
    return header + 1;
}

#define new_callback(type) alloc_callback((loop), &type##_callback_funcs, sizeof(struct type##_callback_data))
#define callback_get_private(data) (((const struct callback_header *)(data)-1)->loop)
#define callback_get_funcs(data) (((const struct callback_header *)(data)-1)->funcs)

static void
destroy_callback (gpointer data)
{
    struct callback_header *header = data;
    MilterLibeventEventLoopPrivate *priv;
    const struct callback_funcs *funcs;
    --header;
    priv = MILTER_LIBEVENT_EVENT_LOOP_GET_PRIVATE(header->loop);
    funcs = header->funcs;
    funcs->stop(priv->base, data);
    g_free(header);
}

static void
run (MilterEventLoop *loop)
{
    MilterLibeventEventLoopPrivate *priv;

    priv = MILTER_LIBEVENT_EVENT_LOOP_GET_PRIVATE(loop);
    ev_loop(priv->base, EVLOOP_NONBLOCK);
}

static gboolean
iterate (MilterEventLoop *loop, gboolean may_block)
{
    MilterLibeventEventLoopPrivate *priv;

    /* TODO: may_block */
    priv = MILTER_LIBEVENT_EVENT_LOOP_GET_PRIVATE(loop);
    ev_loop(priv->base, EVLOOP_ONESHOT);
    return TRUE;                /* TODO */
}

static void
quit (MilterEventLoop *loop)
{
    MilterLibeventEventLoopPrivate *priv;

    priv = MILTER_LIBEVENT_EVENT_LOOP_GET_PRIVATE(loop);
    ev_unloop(priv->base, EVUNLOOP_ONE);
}

static short
evcond_from_g_io_condition(GIOCondition condition)
{
    short event_condition = 0;
    if (condition & (G_IO_IN | G_IO_PRI)) {
        event_condition |= EV_READ;
    }
    if (condition & (G_IO_OUT)) {
        event_condition |= EV_WRITE;
    }
    return event_condition;
}

static GIOCondition
evcond_to_g_io_condition(short event)
{
    GIOCondition condition = 0;
    if (event & EV_READ) {
        condition |= G_IO_IN;
    }
    if (event & EV_WRITE) {
        condition |= G_IO_OUT;
    }
    return condition;
}

struct io_callback_data {
    ev_io event;
    GIOChannel *channel;
    GIOFunc function;
    void *user_data;
};

static void
io_func (struct ev_loop *loop, ev_io *w, int revents)
{
    struct io_callback_data *cb = (struct io_callback_data *)w;

    if (!cb->function(cb->channel, evcond_to_g_io_condition(revents), cb->user_data)) {
        ev_io_stop(loop, w);
        g_free(cb);
    }
}

static const struct callback_funcs io_callback_funcs = {
    (void (*)(struct ev_loop *, void *))ev_io_stop,
};

static guint
watch_io (MilterEventLoop *loop,
          GIOChannel      *channel,
          GIOCondition     condition,
          GIOFunc          function,
          gpointer         data)
{
    int fd = g_io_channel_unix_get_fd(channel);
    struct io_callback_data *cb;
    MilterLibeventEventLoopPrivate *priv;

    priv = MILTER_LIBEVENT_EVENT_LOOP_GET_PRIVATE(loop);
    if (fd == -1) return 0;
    cb = new_callback(io);
    cb->channel = channel;
    cb->function = function;
    cb->user_data = data;
    ev_io_init(&cb->event, io_func, fd, evcond_from_g_io_condition(condition));
    ev_io_start(priv->base, &cb->event);
    return add_callback(priv, cb);
}

struct child_callback_data {
    ev_child event;
    GChildWatchFunc  function;
    void            *user_data;
    GDestroyNotify   notify;
};

static void
child_func (struct ev_loop *loop, ev_child *w, int revents)
{
    struct child_callback_data *cb = (struct child_callback_data *)w;

    cb->function((GPid)w->rpid, w->rstatus, cb->user_data);
    ev_child_stop(loop, w);
    g_free(cb);
}

static const struct callback_funcs child_callback_funcs = {
    (void (*)(struct ev_loop *, void *))ev_child_stop,
};

static guint
watch_child_full (MilterEventLoop *loop,
                  gint             priority,
                  GPid             pid,
                  GChildWatchFunc  function,
                  gpointer         data,
                  GDestroyNotify   notify)
{
    struct child_callback_data *cb;
    MilterLibeventEventLoopPrivate *priv;

    priv = MILTER_LIBEVENT_EVENT_LOOP_GET_PRIVATE(loop);
    cb = new_callback(child);
    cb->function = function;
    cb->user_data = data;
    cb->notify = notify;
    ev_child_init(&cb->event, child_func, pid, FALSE);
    ev_child_start(priv->base, &cb->event);
    return add_callback(priv, cb);
}

struct timer_callback_data {
    ev_timer event;
    GSourceFunc function;
    void *user_data;
};

static void
timer_func (struct ev_loop *loop, ev_timer *w, int revents)
{
    struct timer_callback_data *cb = (struct timer_callback_data *)w;
    if (!cb->function(cb->user_data)) {
        ev_timer_stop(loop, w);
        g_free(cb);
    }
}

static const struct callback_funcs timer_callback_funcs = {
    (void (*)(struct ev_loop *, void *))ev_timer_stop,
};

static guint
add_timeout_full (MilterEventLoop *loop,
                  gint             priority,
                  gdouble          interval_in_seconds,
                  GSourceFunc      function,
                  gpointer         data,
                  GDestroyNotify   notify)
{
    struct timer_callback_data *cb;
    MilterLibeventEventLoopPrivate *priv;

    priv = MILTER_LIBEVENT_EVENT_LOOP_GET_PRIVATE(loop);
    if (interval_in_seconds < 0) return 0;
    cb = new_callback(timer);
    cb->function = function;
    cb->user_data = data;
    ev_timer_init(&cb->event, timer_func, interval_in_seconds, interval_in_seconds);
    ev_timer_start(priv->base, &cb->event);
    return add_callback(priv, cb);
}

struct idle_callback_data {
    ev_idle event;
    GSourceFunc function;
    void *user_data;
};

static void
idle_func (struct ev_loop *loop, ev_idle *w, int revents)
{
    struct idle_callback_data *cb = (struct idle_callback_data *)w;
    if (!cb->function(cb->user_data)) {
        ev_idle_stop(loop, w);
        g_free(cb);
    }
}

static const struct callback_funcs idle_callback_funcs = {
    (void (*)(struct ev_loop *, void *))ev_idle_stop,
};

static guint
add_idle_full (MilterEventLoop *loop,
               gint             priority,
               GSourceFunc      function,
               gpointer         data,
               GDestroyNotify   notify)
{
    struct idle_callback_data *cb;
    MilterLibeventEventLoopPrivate *priv;

    priv = MILTER_LIBEVENT_EVENT_LOOP_GET_PRIVATE(loop);
    cb = new_callback(idle);
    cb->function = function;
    cb->user_data = data;
    ev_idle_init(&cb->event, idle_func);
    ev_idle_start(priv->base, &cb->event);
    return add_callback(priv, cb);
}

static gboolean
remove (MilterEventLoop *loop,
        guint            tag)
{
    MilterLibeventEventLoopPrivate *priv;

    priv = MILTER_LIBEVENT_EVENT_LOOP_GET_PRIVATE(loop);
    return g_hash_table_remove(priv->callbacks, (gconstpointer)(gsize)tag);
}

/*
vi:ts=4:nowrap:ai:expandtab:sw=4
*/