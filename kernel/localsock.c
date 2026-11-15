/*
 * Local stream sockets (see localsock.h). A connection (struct lconn) holds
 * two byte streams; each endpoint (struct local) writes one and reads the
 * other. A listener keeps a ring of connections waiting to be accepted and
 * a place in a small global registry of names. (45b) A packet connection
 * (SOCK_SEQPACKET) keeps where each message ends (struct lstream's ends):
 * a send puts a whole message in or waits, a receive takes one message
 * (what does not fit in the reader's buffer is dropped: MSG_TRUNC).
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
#define PACKET_BUF      LOCAL_PACKET_MAX    /* (45b) a packet connection's: its largest message */
#define PACKET_MESSAGES 256         /* (45b) messages waiting per direction */
#define MAX_LISTENERS   32          /* names that can listen at once */
#define BUFQ            8           /* shared buffers that can wait, per direction */

/* (43b-2) Things passed with the bytes (sendmsg's descriptors): each
 * attachment belongs to the stream's byte at `at` (counted from the
 * connection's start), the first byte of the message it came with. */
#define ATTACHMENTS     16          /* waiting per direction */
struct lattach {
    uint64_t at, end;               /* (end: its message's last byte + 1, as asked) */
    int count;
    void *items[LOCAL_ITEMS_MAX];
};

/* One direction of a connection: a ring with a writer and a reader. */
struct lstream {
    struct spinlock lock;
    struct wait_queue readable;     /* woken when bytes arrive or the writer closes */
    struct wait_queue writable;     /* woken when space frees or the reader leaves */
    uint8_t *buf;
    unsigned head, count;
    bool writer_closed;             /* no more data will be written */
    bool reader_gone;               /* the reading endpoint has left */
    uint64_t written, taken;        /* (43b-2) bytes ever put in and taken out */
    struct lattach *attach;         /* (43b-2) a ring of ATTACHMENTS, made at the first */
    int ahead, acount;
    unsigned size;                  /* (45b) buf's bytes: LOCAL_BUF, or PACKET_BUF */
    uint64_t *ends;                 /* (45b) a packet connection's: a ring of PACKET_MESSAGES */
    int ehead, ecount;              /* messages' ends (counted as written) */
};

/* (43b-2) How an attachment's items are let go of (process.c's: closing
 * a passed file's reference); never called holding a stream's lock. */
static void (*drop_item)(void *item);

void local_set_dropper(void (*drop)(void *item))
{
    drop_item = drop;
}

static void drop_items(void *const *items, int count)
{
    for (int i = 0; i < count; i++)
        if (drop_item)
            drop_item(items[i]);
}

/* Every attachment still waiting on a stream nobody uses any more. */
static void drop_attachments(struct lstream *s)
{
    while (s->attach && s->acount) {
        struct lattach *a = &s->attach[s->ahead];
        drop_items(a->items, a->count);
        s->ahead = (s->ahead + 1) % ATTACHMENTS;
        s->acount--;
    }
    kfree(s->attach);
    s->attach = 0;
}

/* Shared buffers waiting to be received (one queue per direction). */
struct bufq {
    struct spinlock lock;
    struct shbuf *q[BUFQ];
    int head, count;
};

struct lconn {
    int refs;                       /* the two endpoints; freed after the last */
    bool packets;                   /* (45b) SOCK_SEQPACKET: messages kept whole */
    struct lstream c2s;             /* client writes, server reads */
    struct lstream s2c;             /* server writes, client reads */
    struct bufq to_server;          /* buffers the client passed */
    struct bufq to_client;          /* buffers the server passed */
    int client_uid, server_uid;     /* the users at the two ends (local_peer_uid) */
    char client_program[PROGRAM_PATH_MAX];  /* (70c) and their programs (local_peer_program) */
    char server_program[PROGRAM_PATH_MAX];
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
    char program[PROGRAM_PATH_MAX]; /* (70c) and the program it ran */
    bool packets;                   /* (45b) SOCK_SEQPACKET (a listener takes only such) */
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

static void stream_init(struct lstream *s, uint8_t *buf, unsigned size, uint64_t *ends)
{
    s->size = size;
    s->ends = ends;
    s->ehead = s->ecount = 0;
    s->lock = (struct spinlock)SPINLOCK_INIT("local-stream");
    s->readable = (struct wait_queue){ .pollable = true };
    s->writable = (struct wait_queue){ .pollable = true };
    s->buf = buf;
    s->head = s->count = 0;
    s->writer_closed = s->reader_gone = false;
}

static struct lconn *conn_new(bool packets)
{
    unsigned size = packets ? PACKET_BUF : LOCAL_BUF;
    struct lconn *c = kzalloc(sizeof(*c));
    uint8_t *a = kmalloc(size), *b = kmalloc(size);
    uint64_t *ea = packets ? kmalloc(PACKET_MESSAGES * sizeof(uint64_t)) : 0;
    uint64_t *eb = packets ? kmalloc(PACKET_MESSAGES * sizeof(uint64_t)) : 0;
    if (!c || !a || !b || (packets && (!ea || !eb))) {
        kfree(c);
        kfree(a);
        kfree(b);
        kfree(ea);
        kfree(eb);
        return 0;
    }
    c->refs = 2;                    /* the client and the server-to-be */
    c->packets = packets;
    stream_init(&c->c2s, a, size, ea);
    stream_init(&c->s2c, b, size, eb);
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
    drop_attachments(&c->c2s);          /* (43b-2) descriptors nobody received */
    drop_attachments(&c->s2c);
    wait_queue_detach(&c->c2s.readable);    /* (65e) no poller left on them */
    wait_queue_detach(&c->c2s.writable);
    wait_queue_detach(&c->s2c.readable);
    wait_queue_detach(&c->s2c.writable);
    kfree(c->c2s.buf);
    kfree(c->s2c.buf);
    kfree(c->c2s.ends);
    kfree(c->s2c.ends);
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
    r |= out->reader_gone ? POLLERR : out->count < out->size ? POLLOUT : 0;
    spin_unlock(&out->lock, state);
    return r;
}

int local_bind(struct local **l, const char *name, int uid, const char *program, bool packets)
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
    ksnprintf(e->program, sizeof(e->program), "%s", program);
    e->packets = packets;
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

int local_connect(struct local **l, const char *name, int uid, const char *program, bool packets)
{
    if (*l)
        return ERR_INVAL;
    if (!name[0] || strlen(name) >= LOCAL_PATH_MAX)
        return ERR_NAMETOOLONG;
    struct local *client = endpoint_new();
    struct lconn *c = conn_new(packets);
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
    client->packets = packets;
    c->client_uid = uid;
    ksnprintf(c->client_program, sizeof(c->client_program), "%s", program);

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
    } else if (srv->packets != packets) {
        err = ERR_PROTOTYPE;                    /* (45b) a stream to a packet listener, or back */
    } else {
        uint64_t s2 = spin_lock(&srv->lock);
        if (srv->pcount >= srv->backlog) {
            err = ERR_CONNREFUSED;              /* backlog full */
        } else {
            c->server_uid = srv->uid;
            memcpy(c->server_program, srv->program, sizeof(c->server_program));
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
    server->packets = l->packets;
    *out = server;
    return 0;
}

int64_t local_send(struct local *l, const void *data, size_t length, uint32_t timeout_ms)
{
    return local_send_with(l, data, length, timeout_ms, 0, 0);
}

/* (45b) Room for a whole message of length (and its items) on s? */
static bool packet_room(const struct lstream *s, size_t length, int count)
{
    return s->size - s->count >= length && s->ecount < PACKET_MESSAGES &&
           (!count || !s->attach || s->acount < ATTACHMENTS);
}

/* (45b) A packet connection's send: the whole message or nothing; it
 * waits for room (or EAGAIN: O_NONBLOCK, SO_SNDTIMEO). Its items go with
 * it as a stream's do with a send's first byte. */
static int64_t packet_send(struct local *l, const uint8_t *data, size_t length, uint32_t timeout_ms,
                           void *const *items, int count)
{
    struct lstream *s = out_stream(l);
    if (length > s->size)
        return -ERR_MSGSIZE;
    if (!length)
        return 0;                                   /* (no empty messages) */
    struct lattach *ring = 0;
    if (count && !__atomic_load_n(&s->attach, __ATOMIC_ACQUIRE) &&
        !(ring = kzalloc(sizeof(struct lattach) * ATTACHMENTS)))
        return -ERR_NOMEM;
    uint64_t deadline = timeout_ms ? timer_ticks() + ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000
                                   : 0;
    for (;;) {
        uint64_t state = spin_lock(&s->lock);
        if (s->reader_gone) {
            spin_unlock(&s->lock, state);
            kfree(ring);
            return -ERR_PIPE;
        }
        if (packet_room(s, length, count)) {
            if (count) {
                if (!s->attach) {
                    s->attach = ring;
                    ring = 0;
                }
                struct lattach *a = &s->attach[(s->ahead + s->acount) % ATTACHMENTS];
                a->at = s->written;
                a->end = s->written + length;
                a->count = count;
                memcpy(a->items, items, sizeof(void *) * (size_t)count);
                s->acount++;
            }
            for (size_t done = 0; done < length; done++)
                s->buf[(s->head + s->count++) % s->size] = data[done];
            s->written += length;
            s->ends[(s->ehead + s->ecount) % PACKET_MESSAGES] = s->written;
            s->ecount++;
            spin_unlock(&s->lock, state);
            wait_queue_wake_all(&s->readable);
            kfree(ring);
            return (int64_t)length;
        }
        spin_unlock(&s->lock, state);
        bool ready;
        if (timeout_ms) {                           /* (SO_SNDTIMEO) */
            ready = wait_event_until(&s->writable, packet_room(s, length, count) || s->reader_gone ||
                                                   thread_current()->stop_requested, deadline);
        } else if (thread_current()->dont_wait) {
            ready = false;                          /* (O_NONBLOCK) */
        } else {
            ready = wait_event_interruptible(&s->writable, packet_room(s, length, count) ||
                                                           s->reader_gone);
        }
        if (thread_current()->stop_requested) {
            kfree(ring);
            return -ERR_INTR;
        }
        if (!ready) {
            kfree(ring);
            return -ERR_AGAIN;
        }
    }
}

/* (45b) A packet connection's receive: one whole message (its bytes past
 * size dropped: *trunc), with the items sent with it. */
static int64_t packet_recv(struct local *l, void *data, size_t size, uint32_t timeout_ms,
                           void **items, int max, int *got, bool *cut, bool *trunc)
{
    struct lstream *s = in_stream(l);
    uint64_t deadline = timeout_ms ? timer_ticks() + ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000
                                   : 0;
    if (!s->ecount && !s->writer_closed && thread_current()->dont_wait)
        return -ERR_AGAIN;
    if (timeout_ms)
        wait_event_until(&s->readable, s->ecount || s->writer_closed, deadline);
    else
        wait_event_interruptible(&s->readable, s->ecount || s->writer_closed);
    if (thread_current()->stop_requested)
        return -ERR_INTR;
    void *dropped[LOCAL_ITEMS_MAX];
    int ndropped = 0;
    uint64_t state = spin_lock(&s->lock);
    if (!s->ecount) {
        bool eof = s->writer_closed;
        spin_unlock(&s->lock, state);
        return eof ? 0 : -ERR_TIMEDOUT;
    }
    uint64_t length = s->ends[s->ehead] - s->taken;
    if (s->attach && s->acount && s->attach[s->ahead].at == s->taken) {
        struct lattach *a = &s->attach[s->ahead];
        for (int i = 0; i < a->count; i++) {
            if (i < max)
                items[(*got)++] = a->items[i];
            else
                dropped[ndropped++] = a->items[i];
        }
        if (cut && a->count > max && max)
            *cut = true;
        s->ahead = (s->ahead + 1) % ATTACHMENTS;
        s->acount--;
    }
    uint8_t *out = data;
    size_t n = length < size ? (size_t)length : size;
    for (uint64_t i = 0; i < length; i++) {
        if (i < n)
            out[i] = s->buf[s->head];
        s->head = (s->head + 1) % s->size;
    }
    s->count -= (unsigned)length;
    s->taken += length;
    s->ehead = (s->ehead + 1) % PACKET_MESSAGES;
    s->ecount--;
    if (trunc && length > size)
        *trunc = true;
    spin_unlock(&s->lock, state);
    drop_items(dropped, ndropped);
    wait_queue_wake_all(&s->writable);
    return (int64_t)n;
}

/* (43b-2) items (count of them) go with the first byte sent: taken by this
 * call when it returns a length; else the caller's still. */
int64_t local_send_with(struct local *l, const void *data, size_t length, uint32_t timeout_ms,
                        void *const *items, int count)
{
    if (!l->conn)
        return -ERR_NOTCONN;
    if (count < 0 || count > LOCAL_ITEMS_MAX || (count && !length))
        return -ERR_INVAL;
    if (l->conn->packets)
        return packet_send(l, data, length, timeout_ms, items, count);
    struct lstream *s = out_stream(l);
    struct lattach *ring = 0;
    if (count && !__atomic_load_n(&s->attach, __ATOMIC_ACQUIRE) &&
        !(ring = kzalloc(sizeof(struct lattach) * ATTACHMENTS)))
        return -ERR_NOMEM;
    const uint8_t *p = data;
    size_t done = 0;
    uint64_t deadline = timeout_ms ? timer_ticks() + ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000
                                   : 0;
    while (done < length) {
        if (timeout_ms) {                           /* (53a: SO_SNDTIMEO) */
            if (!wait_event_until(&s->writable, s->count < s->size || s->reader_gone ||
                                                thread_current()->stop_requested, deadline))
                return done ? (int64_t)done : -ERR_AGAIN;
        } else if (s->count >= s->size && !s->reader_gone && thread_current()->dont_wait) {
            return done ? (int64_t)done : -ERR_AGAIN;   /* (53b) */
        } else {
            wait_event_interruptible(&s->writable, s->count < s->size || s->reader_gone);
        }
        if (thread_current()->stop_requested)
            return done ? (int64_t)done : -ERR_INTR;
        uint64_t state = spin_lock(&s->lock);
        if (s->reader_gone) {
            spin_unlock(&s->lock, state);
            kfree(ring);
            return done ? (int64_t)done : -ERR_PIPE;
        }
        if (count && !done) {                       /* (43b-2) with the first byte */
            if (!s->attach) {
                s->attach = ring;
                ring = 0;
            }
            if (s->acount == ATTACHMENTS) {
                spin_unlock(&s->lock, state);
                kfree(ring);
                return -ERR_AGAIN;                  /* (too many unreceived: try later) */
            }
            struct lattach *a = &s->attach[(s->ahead + s->acount) % ATTACHMENTS];
            a->at = s->written;
            a->end = s->written + length;
            a->count = count;
            memcpy(a->items, items, sizeof(void *) * (size_t)count);
            s->acount++;
        }
        while (done < length && s->count < s->size) {
            s->buf[(s->head + s->count) % s->size] = p[done++];
            s->count++;
            s->written++;
        }
        spin_unlock(&s->lock, state);
        wait_queue_wake_all(&s->readable);
    }
    kfree(ring);
    return (int64_t)done;
}

int64_t local_recv(struct local *l, void *data, size_t size, uint32_t timeout_ms)
{
    return local_recv_with(l, data, size, timeout_ms, 0, 0, 0, 0, 0);
}

/* (43b-2) Also the items attached at the start of what is read (into
 * items, at most max: *got of them; the rest let go of, *cut set); a read
 * stops before the next attachment, so each comes at a read's start. A
 * read that takes no items (max 0) lets the ones at its start go. */
int64_t local_recv_with(struct local *l, void *data, size_t size, uint32_t timeout_ms,
                        void **items, int max, int *got, bool *cut, bool *trunc)
{
    int unused_got;
    if (!got)
        got = &unused_got;
    *got = 0;
    if (cut)
        *cut = false;
    if (trunc)
        *trunc = false;
    if (!l->conn)
        return -ERR_NOTCONN;
    if (l->read_shut)
        return 0;
    if (l->conn->packets)
        return packet_recv(l, data, size, timeout_ms, items, max, got, cut, trunc);   /* (45b) */
    if (size == 0)
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
    /* (43b-2) the attachment at this read's start is taken; the read stops
     * before the next one */
    void *dropped[LOCAL_ITEMS_MAX];
    int ndropped = 0;
    size_t limit = size;
    if (s->attach && s->acount && s->attach[s->ahead].at == s->taken) {
        struct lattach *a = &s->attach[s->ahead];
        if (a->end - s->taken < limit)
            limit = (size_t)(a->end - s->taken);    /* (and stops at its message's end, as Linux) */
        for (int i = 0; i < a->count; i++) {
            if (i < max)
                items[(*got)++] = a->items[i];
            else
                dropped[ndropped++] = a->items[i];
        }
        if (cut && a->count > max && max)
            *cut = true;
        s->ahead = (s->ahead + 1) % ATTACHMENTS;
        s->acount--;
    }
    if (s->attach && s->acount && s->attach[s->ahead].at - s->taken < limit)
        limit = (size_t)(s->attach[s->ahead].at - s->taken);
    uint8_t *out = data;
    size_t n = 0;
    while (n < limit && s->count) {
        out[n++] = s->buf[s->head];
        s->head = (s->head + 1) % s->size;
        s->count--;
        s->taken++;
    }
    spin_unlock(&s->lock, state);
    drop_items(dropped, ndropped);
    wait_queue_wake_all(&s->writable);          /* space freed for the writer */
    return (int64_t)n;
}

/* (43b-2) A connected pair with no name (socketpair): *a the client end,
 * *b the server end, both the user uid's. */
int local_pair(struct local **a, struct local **b, int uid, const char *program, bool packets)
{
    struct lconn *c = conn_new(packets);
    struct local *x = endpoint_new(), *y = endpoint_new();
    if (!c || !x || !y) {
        if (c) {
            kfree(c->c2s.buf);
            kfree(c->s2c.buf);
            kfree(c->c2s.ends);
            kfree(c->s2c.ends);
            kfree(c);
        }
        kfree(x);
        kfree(y);
        return ERR_NOMEM;
    }
    c->client_uid = c->server_uid = uid;
    ksnprintf(c->client_program, sizeof(c->client_program), "%s", program);
    memcpy(c->server_program, c->client_program, sizeof(c->server_program));
    x->conn = c;
    y->conn = c;
    y->server = true;
    x->packets = y->packets = packets;
    *a = x;
    *b = y;
    return 0;
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

const char *local_peer_program(struct local *l)
{
    if (!l || !l->conn)
        return 0;
    return l->server ? l->conn->client_program : l->conn->server_program;
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
