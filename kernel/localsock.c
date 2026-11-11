/*
 * Local stream sockets (see localsock.h). A connection (struct lconn) holds
 * two byte streams; each endpoint (struct local) writes one and reads the
 * other. A listener keeps a ring of connections waiting to be accepted and
 * a place in a small global registry of names.
 */
#include "localsock.h"
#include "heap.h"
#include "kprintf.h"
#include "sched.h"
#include "shbuf.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "timer.h"

#define LOCAL_BUF       8192        /* bytes buffered per direction */
#define MAX_LISTENERS   32          /* names that can listen at once */
#define BUFQ            8           /* shared buffers that can wait, per direction */

/* One direction of a connection: a ring with a writer and a reader. */
struct lstream {
    struct spinlock lock;
    struct wait_queue readable;     /* woken when bytes arrive or the writer closes */
    struct wait_queue writable;     /* woken when space frees or the reader leaves */
    uint8_t *buf;
    unsigned head, count;
    bool writer_closed;             /* no more data will be written */
    bool reader_gone;               /* the reading endpoint has left */
};

/* Shared buffers waiting to be received (one queue per direction). */
struct bufq {
    struct spinlock lock;
    struct shbuf *q[BUFQ];
    int head, count;
};

struct lconn {
    int refs;                       /* the two endpoints; freed after the last */
    struct lstream c2s;             /* client writes, server reads */
    struct lstream s2c;             /* server writes, client reads */
    struct bufq to_server;          /* buffers the client passed */
    struct bufq to_client;          /* buffers the server passed */
    int client_uid, server_uid;     /* the users at the two ends (local_peer_uid) */
    char name[LOCAL_PATH_MAX];      /* (53a) the listener's name it was made through */
};

struct local {
    char name[LOCAL_PATH_MAX];
    bool registered;                /* a listener in the registry */
    struct spinlock lock;           /* the backlog */
    struct wait_queue accept_q;
    struct lconn **pending;         /* ring of connections waiting for accept */
    int backlog, phead, pcount;
    struct lconn *conn;             /* a connected endpoint */
    bool server;                    /* which streams it reads and writes */
    bool read_shut;                 /* (53a) shutdown(SHUT_RD): reads give the end */
    int uid;                        /* a listener: the user who bound it */
};

/* The registry of listening names. */
static struct local *listeners[MAX_LISTENERS];
static struct spinlock registry = SPINLOCK_INIT("local-registry");

static struct lstream *out_stream(struct local *l)
{
    return l->server ? &l->conn->s2c : &l->conn->c2s;
}

static struct lstream *in_stream(struct local *l)
{
    return l->server ? &l->conn->c2s : &l->conn->s2c;
}

static void stream_init(struct lstream *s, uint8_t *buf)
{
    s->lock = (struct spinlock)SPINLOCK_INIT("local-stream");
    s->readable = (struct wait_queue){ .pollable = true };
    s->writable = (struct wait_queue){ .pollable = true };
    s->buf = buf;
    s->head = s->count = 0;
    s->writer_closed = s->reader_gone = false;
}

static struct lconn *conn_new(void)
{
    struct lconn *c = kzalloc(sizeof(*c));
    uint8_t *a = kmalloc(LOCAL_BUF), *b = kmalloc(LOCAL_BUF);
    if (!c || !a || !b) {
        kfree(c);
        kfree(a);
        kfree(b);
        return 0;
    }
    c->refs = 2;                    /* the client and the server-to-be */
    stream_init(&c->c2s, a);
    stream_init(&c->s2c, b);
    c->to_server.lock = (struct spinlock)SPINLOCK_INIT("local-bufq");
    c->to_client.lock = (struct spinlock)SPINLOCK_INIT("local-bufq");
    return c;
}

static void drain_bufq(struct bufq *q);
static void hang_up(struct lconn *c, bool server);

static void conn_unref(struct lconn *c)
{
    if (__atomic_sub_fetch(&c->refs, 1, __ATOMIC_SEQ_CST) > 0)
        return;
    drain_bufq(&c->to_server);          /* buffers nobody received */
    drain_bufq(&c->to_client);
    wait_queue_detach(&c->c2s.readable);    /* (65e) no poller left on them */
    wait_queue_detach(&c->c2s.writable);
    wait_queue_detach(&c->s2c.readable);
    wait_queue_detach(&c->s2c.writable);
    kfree(c->c2s.buf);
    kfree(c->s2c.buf);
    kfree(c);
}

static struct local *endpoint_new(void)
{
    struct local *l = kzalloc(sizeof(*l));
    if (l) {
        l->lock = (struct spinlock)SPINLOCK_INIT("local");
        l->accept_q.pollable = true;
    }
    return l;
}

int local_sock_name(struct local *l, bool peer, char *out, size_t size)
{
    out[0] = 0;
    if (!l->conn) {                             /* bound or listening: its own name */
        if (peer)
            return ERR_NOTCONN;
        ksnprintf(out, size, "%s", l->name);
        return 0;
    }
    if (peer != l->server)                      /* the client's peer, the server's own */
        ksnprintf(out, size, "%s", l->conn->name);
    return 0;
}

void local_shutdown(struct local *l, bool read, bool write)
{
    if (!l->conn)
        return;
    if (write) {
        struct lstream *out = out_stream(l);
        uint64_t state = spin_lock(&out->lock);
        out->writer_closed = true;
        spin_unlock(&out->lock, state);
        wait_queue_wake_all(&out->readable);    /* the peer reads the end */
    }
    if (read) {
        struct lstream *in = in_stream(l);
        l->read_shut = true;
        uint64_t state = spin_lock(&in->lock);
        in->reader_gone = true;
        spin_unlock(&in->lock, state);
        wait_queue_wake_all(&in->writable);     /* the peer's writes fail */
        wait_queue_wake_all(&in->readable);
    }
}

int local_poll(struct local *l)
{
    if (l->registered)
        poll_watch(&l->accept_q);           /* (65e) before the state is read */
    else if (l->conn) {
        poll_watch(&in_stream(l)->readable);
        poll_watch(&out_stream(l)->writable);
        poll_watch(&in_stream(l)->writable);
        poll_watch(&out_stream(l)->readable);
    }
    if (l->registered) {
        uint64_t state = spin_lock(&l->lock);
        int r = l->pcount > 0 ? POLLIN : 0;
        spin_unlock(&l->lock, state);
        return r;
    }
    if (!l->conn)
        return 0;
    struct lstream *in = in_stream(l), *out = out_stream(l);
    uint64_t state = spin_lock(&in->lock);
    int r = (in->count || in->writer_closed ? POLLIN : 0) | (in->writer_closed ? POLLHUP : 0);
    spin_unlock(&in->lock, state);
    state = spin_lock(&out->lock);
    r |= out->reader_gone ? POLLERR : out->count < LOCAL_BUF ? POLLOUT : 0;
    spin_unlock(&out->lock, state);
    return r;
}

int local_bind(struct local **l, const char *name, int uid)
{
    if (*l)
        return ERR_INVAL;                       /* bound already */
    if (!name[0] || strlen(name) >= LOCAL_PATH_MAX)
        return ERR_NAMETOOLONG;
    struct local *e = endpoint_new();
    if (!e)
        return ERR_NOMEM;
    ksnprintf(e->name, sizeof(e->name), "%s", name);
    e->uid = uid;
    *l = e;
    return 0;
}

int local_listen(struct local **l, int backlog)
{
    struct local *e = *l;
    if (!e || !e->name[0] || e->registered || e->conn)
        return ERR_INVAL;
    if (backlog < 1)
        backlog = 1;
    if (backlog > 128)
        backlog = 128;
    e->pending = kmalloc(backlog * sizeof(struct lconn *));
    if (!e->pending)
        return ERR_NOMEM;
    e->backlog = backlog;
    uint64_t state = spin_lock(&registry);
    int slot = -1;
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (listeners[i] && strcmp(listeners[i]->name, e->name) == 0) {
            spin_unlock(&registry, state);
            kfree(e->pending);
            e->pending = 0;
            return ERR_ADDRINUSE;
        }
        if (!listeners[i] && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        spin_unlock(&registry, state);
        kfree(e->pending);
        e->pending = 0;
        return ERR_AGAIN;
    }
    listeners[slot] = e;
    e->registered = true;
    spin_unlock(&registry, state);
    return 0;
}

int local_connect(struct local **l, const char *name, int uid)
{
    if (*l)
        return ERR_INVAL;
    if (!name[0] || strlen(name) >= LOCAL_PATH_MAX)
        return ERR_NAMETOOLONG;
    struct local *client = endpoint_new();
    struct lconn *c = conn_new();
    if (!client || !c) {
        kfree(client);
        if (c) {
            c->refs = 1;
            conn_unref(c);
        }
        return ERR_NOMEM;
    }
    client->conn = c;
    client->server = false;
    c->client_uid = uid;

    /* Find the listener and queue the connection, all under the registry
     * lock so the listener cannot be freed in between. */
    uint64_t state = spin_lock(&registry);
    struct local *srv = 0;
    for (int i = 0; i < MAX_LISTENERS; i++)
        if (listeners[i] && strcmp(listeners[i]->name, name) == 0) {
            srv = listeners[i];
            break;
        }
    int err = 0;
    if (!srv) {
        err = ERR_CONNREFUSED;
    } else {
        uint64_t s2 = spin_lock(&srv->lock);
        if (srv->pcount >= srv->backlog) {
            err = ERR_CONNREFUSED;              /* backlog full */
        } else {
            c->server_uid = srv->uid;
            ksnprintf(c->name, sizeof(c->name), "%s", srv->name);
            srv->pending[(srv->phead + srv->pcount) % srv->backlog] = c;
            srv->pcount++;
        }
        spin_unlock(&srv->lock, s2);
        if (!err)
            wait_queue_wake_all(&srv->accept_q);
    }
    spin_unlock(&registry, state);
    if (err) {
        client->conn = 0;
        kfree(client);
        c->refs = 1;                            /* only this reference exists */
        conn_unref(c);
        return err;
    }
    *l = client;
    return 0;
}

int local_accept(struct local *l, struct local **out)
{
    if (!l->registered)
        return ERR_INVAL;
    if (l->pcount == 0 && thread_current()->dont_wait)
        return ERR_AGAIN;                           /* (53b) */
    if (!wait_event_interruptible(&l->accept_q,
                                  l->pcount > 0 || thread_current()->stop_requested))
        return ERR_INTR;
    if (thread_current()->stop_requested)
        return ERR_INTR;
    uint64_t state = spin_lock(&l->lock);
    if (l->pcount == 0) {
        spin_unlock(&l->lock, state);
        return ERR_AGAIN;
    }
    struct lconn *c = l->pending[l->phead];
    l->phead = (l->phead + 1) % l->backlog;
    l->pcount--;
    spin_unlock(&l->lock, state);
    struct local *server = endpoint_new();
    if (!server) {
        hang_up(c, true);                       /* tell the client the server is gone */
        conn_unref(c);                          /* the server ref goes */
        return ERR_NOMEM;
    }
    server->conn = c;
    server->server = true;
    *out = server;
    return 0;
}

int64_t local_send(struct local *l, const void *data, size_t length, uint32_t timeout_ms)
{
    if (!l->conn)
        return -ERR_NOTCONN;
    struct lstream *s = out_stream(l);
    const uint8_t *p = data;
    size_t done = 0;
    uint64_t deadline = timeout_ms ? timer_ticks() + ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000
                                   : 0;
    while (done < length) {
        if (timeout_ms) {                           /* (53a: SO_SNDTIMEO) */
            if (!wait_event_until(&s->writable, s->count < LOCAL_BUF || s->reader_gone ||
                                                thread_current()->stop_requested, deadline))
                return done ? (int64_t)done : -ERR_AGAIN;
        } else if (s->count >= LOCAL_BUF && !s->reader_gone && thread_current()->dont_wait) {
            return done ? (int64_t)done : -ERR_AGAIN;   /* (53b) */
        } else {
            wait_event_interruptible(&s->writable, s->count < LOCAL_BUF || s->reader_gone);
        }
        if (thread_current()->stop_requested)
            return done ? (int64_t)done : -ERR_INTR;
        uint64_t state = spin_lock(&s->lock);
        if (s->reader_gone) {
            spin_unlock(&s->lock, state);
            return done ? (int64_t)done : -ERR_PIPE;
        }
        while (done < length && s->count < LOCAL_BUF) {
            s->buf[(s->head + s->count) % LOCAL_BUF] = p[done++];
            s->count++;
        }
        spin_unlock(&s->lock, state);
        wait_queue_wake_all(&s->readable);
    }
    return (int64_t)done;
}

int64_t local_recv(struct local *l, void *data, size_t size, uint32_t timeout_ms)
{
    if (!l->conn)
        return -ERR_NOTCONN;
    if (size == 0 || l->read_shut)
        return 0;
    struct lstream *s = in_stream(l);
    uint64_t deadline = timeout_ms ? timer_ticks() + ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000
                                   : 0;
    if (!s->count && !s->writer_closed && thread_current()->dont_wait)
        return -ERR_AGAIN;                          /* (53b) */
    bool ready = timeout_ms
        ? wait_event_until(&s->readable, s->count || s->writer_closed, deadline)
        : wait_event_interruptible(&s->readable, s->count || s->writer_closed);
    if (thread_current()->stop_requested)
        return -ERR_INTR;
    uint64_t state = spin_lock(&s->lock);
    if (s->count == 0) {
        bool eof = s->writer_closed;
        spin_unlock(&s->lock, state);
        return eof ? 0 : -ERR_TIMEDOUT;         /* !ready: the deadline passed */
    }
    (void)ready;
    uint8_t *out = data;
    size_t n = 0;
    while (n < size && s->count) {
        out[n++] = s->buf[s->head];
        s->head = (s->head + 1) % LOCAL_BUF;
        s->count--;
    }
    spin_unlock(&s->lock, state);
    wait_queue_wake_all(&s->writable);          /* space freed for the writer */
    return (int64_t)n;
}

int local_send_buffer(struct local *l, struct shbuf *b)
{
    if (!l->conn)
        return ERR_NOTCONN;
    struct bufq *q = l->server ? &l->conn->to_client : &l->conn->to_server;
    uint64_t state = spin_lock(&q->lock);
    if (q->count >= BUFQ) {
        spin_unlock(&q->lock, state);
        return ERR_AGAIN;
    }
    q->q[(q->head + q->count) % BUFQ] = b;
    q->count++;
    spin_unlock(&q->lock, state);
    return 0;
}

struct shbuf *local_recv_buffer(struct local *l)
{
    if (!l->conn)
        return 0;
    struct bufq *q = l->server ? &l->conn->to_server : &l->conn->to_client;
    uint64_t state = spin_lock(&q->lock);
    struct shbuf *b = 0;
    if (q->count) {
        b = q->q[q->head];
        q->head = (q->head + 1) % BUFQ;
        q->count--;
    }
    spin_unlock(&q->lock, state);
    return b;
}

static void drain_bufq(struct bufq *q)
{
    while (q->count) {
        shbuf_unref(q->q[q->head]);
        q->head = (q->head + 1) % BUFQ;
        q->count--;
    }
}

const char *local_name(struct local *l)
{
    return l->name;
}

int local_peer_uid(struct local *l)
{
    if (!l || !l->conn)
        return -1;
    return l->server ? l->conn->client_uid : l->conn->server_uid;
}

/* Tell the peer this endpoint is gone: no more data will arrive on our
 * outgoing stream, and nothing more will be read from our incoming one. */
static void hang_up(struct lconn *c, bool server)
{
    struct lstream *out = server ? &c->s2c : &c->c2s;
    struct lstream *in = server ? &c->c2s : &c->s2c;
    uint64_t state = spin_lock(&out->lock);
    out->writer_closed = true;
    spin_unlock(&out->lock, state);
    state = spin_lock(&in->lock);
    in->reader_gone = true;
    spin_unlock(&in->lock, state);
    wait_queue_wake_all(&out->readable);        /* the peer's recv sees EOF */
    wait_queue_wake_all(&in->writable);         /* the peer's send sees EPIPE */
}

void local_close(struct local *l)
{
    if (l->registered) {
        uint64_t state = spin_lock(&registry);
        for (int i = 0; i < MAX_LISTENERS; i++)
            if (listeners[i] == l) {
                listeners[i] = 0;
                break;
            }
        spin_unlock(&registry, state);
        /* Refuse the connections still waiting: hang up their client side
         * and drop the server reference that was reserved for each. */
        state = spin_lock(&l->lock);
        while (l->pcount) {
            struct lconn *c = l->pending[l->phead];
            l->phead = (l->phead + 1) % l->backlog;
            l->pcount--;
            spin_unlock(&l->lock, state);
            hang_up(c, true);
            conn_unref(c);
            state = spin_lock(&l->lock);
        }
        spin_unlock(&l->lock, state);
        kfree(l->pending);
    }
    if (l->conn) {
        hang_up(l->conn, l->server);
        conn_unref(l->conn);
    }
    wait_queue_detach(&l->accept_q);        /* (65e) no poller left on it */
    kfree(l);
}
