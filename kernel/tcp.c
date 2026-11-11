/*
 * TCP (RFC 793, 1122, 6298): reliable byte streams over IPv4 and (64c)
 * IPv6: addresses are struct inet_addr (IPv4 as ::ffff:a.b.c.d), so one
 * table and one state machine serve both.
 *
 * Each byte has a sequence number. A connection starts with a three-way
 * handshake (SYN, SYN+ACK, ACK) in which both sides choose their first
 * number; data is acknowledged by the receiver (ACK: the next number it
 * expects) and sent again by the sender if no acknowledgement comes in
 * time (the retransmission timeout, doubled at each attempt). The
 * receiver's window says how much more it can take; the sender never has
 * more outstanding. FIN ends each direction; the side that closed first
 * waits a while (TIME_WAIT) so that late segments of this connection
 * cannot be taken for a new one's.
 *
 * Losses: a segment that arrives after a gap is kept (in the receive
 * ring, at its place) and acknowledged at once with what is still
 * expected; three such duplicate acknowledgements make the sender send
 * the missing segment at once (fast retransmit) and halve its congestion
 * window instead of starting over (fast recovery, RFC 5681 and 6582's
 * NewReno for several losses in one window). A closed window (the
 * receiver's buffer full) is asked about with one byte from time to time
 * (the persist timer), since its reopening might be lost.
 *
 * (65e) Window scaling (RFC 7323): our SYN offers a shift of RCV_SHIFT
 * for a receive buffer that starts at RECV_START and grows to RECV_BIG
 * while the window holds the sender back (65e-18, grow_buffer); if the
 * answer offers one too, both windows are scaled (the peer's by its shift).
 * (65e-25) Connections to us too: a listener keeps no state for a SYN, so
 * its cookie carries the peer's shift and SACK offer (make_cookie), and
 * its SYN-ACK offers each only if the SYN did.
 *
 * (65e) Selective acknowledgements (RFC 2018), the receiving side: the
 * same SYN offers SACK; if the answer does too, an ACK while data beyond a
 * gap is kept reports up to SACK_BLOCKS of those ranges (the newest first),
 * so the sender resends only what is missing (without them it learns of one
 * missing piece per round trip). (65e-19) The sending side too: the peer's
 * SACK blocks go on a scoreboard (SCORE ranges above snd_una), and in fast
 * recovery each duplicate or partial ACK sends the next hole below its
 * highest SACKed byte, once (RFC 6675, simplified: NewReno's window
 * arithmetic, SACK's choice of what to resend), instead of one hole per
 * round trip; a timeout forgets the scoreboard and goes back as before.
 *
 * (65e-35) Locks: "tcp table" guards the slots (taking and freeing one),
 * the ports, the matching of a segment to its connection and the
 * listeners' queues of connections to accept; "tcp connection", one in
 * each connection, everything else of it. The table's comes first, never
 * after a connection's; a slot is freed only with both held (so one found
 * under the table's lock stays itself once its own lock is taken). Until
 * 65e-35 one mutex guarded all of it: the Pluto's stream (the card's
 * receive thread and plutosa's reader on one connection, sshd on another)
 * waited for it 2.1 million times in 14 minutes (718 ms). Each connection
 * sends its segments after its lock is given back (its own outbox).
 *
 * Simplifications: at most 32 connections; no urgent data, no timestamps. "netloss=N" on the
 * kernel command line drops one segment in N, coming and going (tests).
 */
#include "tcp.h"
#include "arch.h"
#include "heap.h"
#include "inet.h"
#include "ipv4.h"
#include "kprintf.h"
#include "net.h"
#include "random.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "timer.h"

#define CONNECTIONS     32
#define BUFFER          65536           /* sending: what every connection starts with */
#define SEND_BIG        (4u << 20)      /* (65e) growing to 4 MiB (send_grow) */
#define RECV_BUFFER     65536           /* (65e) receiving: what every connection starts with */
#define RECV_START      (128u << 10)    /* (65e) a connection we open starts with 128 KiB, */
#define RECV_BIG        (4u << 20)      /* growing to 4 MiB (always 4 MiB in 65e-17) */
#define RCV_SHIFT       6               /* (65e) our window's shift: 65535 << 6 is ~4 MiB */
#define SHIFT_MAX       14              /* RFC 7323's largest shift */
#define MSS             1460            /* the most data in one segment (Ethernet) */
#define TICK_MS         100             /* the tcp thread's step */
#define RTO_INITIAL     1000            /* ms, until a round trip was measured */
#define RTO_MIN         200
#define RTO_MAX         60000
#define RETRIES         8               /* then the connection is given up */
#define TIME_WAIT_MS    2000            /* 2 MSL, shortened */
#define FIN_WAIT2_MS    5000            /* a peer that never sends its FIN: free the slot (49e) */
#define BACKLOG_MAX     8
#define GAPS            64              /* ranges kept beyond a gap (65e: was 8) */
#define SACK_BLOCKS     4               /* (65e) most an ACK reports (40 option bytes) */
#define SCORE           16              /* (65e) ranges the peer SACKed, kept when sending */
#define DELACK_MS       40              /* (65e-29) the longest an ACK is held (Linux's least) */
#define QUICK_SEGMENTS  16              /* (65e-29) a connection's first: acknowledged at once */

#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

enum state { CLOSED, LISTEN, SYN_SENT, SYN_RECEIVED, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2,
             CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT };

/* (65e) Segments are sent after the lock is given back: until 65e each one
 * went through IP and the card's driver with it held (an ACK for every
 * segment received: on a USB card a bulk transfer), and every reader and
 * writer of every connection waited meanwhile (the laptop: the card's
 * receive thread and plutosa's reader met 4,600 times a second). The
 * outbox keeps them in order; one thread at a time sends from it. (65e-35)
 * Each connection has its own, made at its first segment; a reply that
 * belongs to no connection (a reset, a SYN cookie's SYN-ACK) goes into one
 * of a single segment on the stack, sent once the table's lock is free. */
#define OUTBOX 64
struct outgoing {
    struct inet_addr src, dst;
    uint16_t length;
    uint8_t bytes[20 + 4 + MSS];
};
struct outbox {
    struct outgoing *ring;              /* size of them (0: none yet) */
    unsigned size, head, count;
    bool sending;                       /* a thread sends from it now */
};

struct tcp {
    struct mutex lock;                  /* (65e-35) the connection's; kept when the slot is freed */
    enum state state;
    bool used;                          /* the slot is taken */
    bool ack_owed;                      /* (65e) in-order data not acknowledged yet, */
    struct thread *owed_by;             /* (65e-35) by this thread's burst (tcp_batch_end) */
    /* (65e-29) Delayed ACKs: full-sized segments not acknowledged yet (at
     * most one waits: RFC 1122's every second one), something owed that may
     * not wait (a short segment, a gap filled, the first segments), and
     * when the timer sends the held ACK (0: none held). */
    uint8_t full_owed;
    bool ack_soon;
    uint64_t delack_at;                 /* ms */
    bool user_closed;                   /* the program closed it: free when done */
    struct inet_addr local, remote;
    uint16_t local_port, remote_port;
    unsigned families;                  /* INET_V4, INET_V6: the peers it takes (64c, 64d) */
    uint32_t own_mss;                   /* the most data a segment of ours may carry (the
                                           link's, lowered by "packet too big") */
    /* Sending: snd_una is the oldest byte not acknowledged, snd_nxt the
     * next to send; the bytes between them and the ones after, waiting,
     * are in the send ring from una on. */
    uint32_t iss, snd_una, snd_nxt, snd_wnd, peer_mss;
    uint8_t snd_shift, rcv_shift;       /* (65e) the peer's window shift, ours (0: not scaled) */
    bool ws_offer, sack_offer;          /* (65e) our SYN offers window scaling, SACK */
    bool sack_ok;                       /* (65e) both offered SACK: our ACKs report the gaps */
    uint32_t send_size;                 /* (65e) send_buf's size: BUFFER, doubling up to SEND_BIG */
    uint32_t recv_size;                 /* (65e) recv_buf's size (RECV_BUFFER, or RECV_START up to
                                         * RECV_BIG, doubling) */
    /* (65e) Growing the buffer (grow_check): the round trip as the receiver
     * sees it (how long a window's worth takes to come, the shortest seen),
     * and what came in the last such time. */
    uint64_t rtt_t0, span_t0;           /* arch_counter */
    uint32_t rtt_mark, span_seq;
    uint64_t rcv_rtt;                   /* arch_counter counts; 0: not measured yet */
    uint32_t snd_max;                   /* the highest sent so far: after a timeout
                                           snd_nxt goes back, acknowledgements may
                                           still reach up to here */
    uint8_t *send_buf;
    uint32_t send_len;                  /* bytes in the ring, from snd_una's */
    bool fin_queued, fin_sent;          /* fin_sent: below snd_nxt */
    bool fin_out;                       /* a FIN went out once, at fin_seq */
    uint32_t fin_seq;
    /* Receiving. */
    uint32_t irs, rcv_nxt;
    uint8_t *recv_buf;
    uint32_t recv_head, recv_len;
    bool peer_fin;
    bool read_shut;                     /* (53a) shutdown(SHUT_RD): reads give the end */
    /* Data beyond a gap: [start, end) in sequence numbers, in the ring
     * after recv_len at their distance from rcv_nxt. */
    uint32_t gap_start[GAPS], gap_end[GAPS];
    int gaps;
    /* Fast retransmit and recovery. */
    uint32_t dupacks, recover;
    /* (65e) The peer's SACK blocks (above snd_una, sorted, merged), and
     * where in recovery the holes resent so far reach. */
    uint32_t score_start[SCORE], score_end[SCORE];
    int scored;
    uint32_t resent_to;
    bool recovering;
    /* Timers (ms of uptime). */
    uint64_t rto_at, time_wait_until;
    uint32_t rto, retries, srtt, rttvar;
    uint64_t rtt_start;                 /* measuring: when rtt_seq went out (0: not) */
    uint32_t rtt_seq;
    uint32_t cwnd, ssthresh;
    int error;                          /* why it ended (ERR_*), for the program */
    /* A listener: connections that finished their handshake. */
    struct tcp *backlog[BACKLOG_MAX];
    int backlog_count, backlog_max;
    struct tcp *listener;               /* not accepted yet: whose backlog it is in (table's lock) */
    struct wait_queue waiters;
    struct outbox box;                  /* (65e-35) its segments waiting to go */
    struct outbox *out;                 /* a reply's box instead (reset_for), else 0: box */
};

static struct tcp table[CONNECTIONS];
static struct {
    uint64_t sent, received, timeouts, fast, probes, kept, lost, too_big, scaled, sacked, grown,
             holes,                         /* (65e) holes resent from the scoreboard */
             sgrown,                        /* (65e) send buffers doubled */
             delayed,                       /* (65e-29) ACKs held for a second segment */
             resets_unread;                 /* (36e-8) RSTs: closed with data unread */
} stats;
static uint32_t loss_one_in;            /* netloss=N; 0: no losses */
static uint32_t loss_state = 2463534242u;
static struct mutex table_lock = MUTEX_INIT("tcp table");

/* (65e-35) The counters, from any connection's lock. */
#define COUNT(field) __atomic_fetch_add(&stats.field, 1, __ATOMIC_RELAXED)

/* (65e-29) The tcp thread sleeps until timer_wake_ms; a held ACK due
 * earlier wakes it (timer_kicked). */
static struct wait_queue timer_wq;
static bool timer_kicked;
static uint64_t timer_wake_ms;

/* (65e) An ACK owed until the end of a card's burst (an NTB): one ACK for
 * all the segments it brought instead of one each. The Pluto's NCM link
 * took 19% of our ACKs only after 1-5 ms, and its TCP, allowed 64 KiB
 * beyond what we had acknowledged, waited for them meanwhile. (65e-35)
 * Marked in the connection (ack_owed, owed_by), not in a shared list. */

/* Give the connection's lock back, then send what waits in its outbox
 * (unless another thread is at it: it sends these too, in order). */
static void release(struct tcp *t)
{
    struct outbox *b = &t->box;
    bool mine = !b->sending && b->count;
    if (mine)
        b->sending = true;
    mutex_unlock(&t->lock);
    while (mine) {
        struct outgoing one;
        mutex_lock(&t->lock);
        if (!b->count) {
            b->sending = false;
            mutex_unlock(&t->lock);
            break;
        }
        one = b->ring[b->head];
        b->head = (b->head + 1) % b->size;
        b->count--;
        mutex_unlock(&t->lock);
        inet_send(&one.src, &one.dst, IP_PROTO_TCP, one.bytes, one.length);
    }
}

/* A reply's one segment, with no lock held. */
static void send_reply(struct outbox *b)
{
    if (b->count)
        inet_send(&b->ring[0].src, &b->ring[0].dst, IP_PROTO_TCP, b->ring[0].bytes,
                  b->ring[0].length);
}

/* t's lock, from the table's (which is given back): t stays itself. */
static void take(struct tcp *t)
{
    mutex_lock(&t->lock);
    mutex_unlock(&table_lock);
}
static bool thread_started;
static uint16_t next_port = 49152;

static uint64_t now_ms(void)
{
    return timer_ticks() * (1000 / TIMER_HZ);
}

/* Sequence numbers wrap: compare by the signed difference. */
static bool before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)get16(p) << 16 | get16(p + 2);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)(v >> 16)), put16(p + 2, (uint16_t)v);
}

/* netloss: one segment in N lost, pseudo-randomly (xorshift: the same
 * losses every time). */
static bool lose(void)
{
    if (!loss_one_in)
        return false;
    uint32_t old = __atomic_load_n(&loss_state, __ATOMIC_RELAXED), v;
    do {                                    /* (65e-35: from any connection's lock) */
        v = old;
        v ^= v << 13;
        v ^= v >> 17;
        v ^= v << 5;
    } while (!__atomic_compare_exchange_n(&loss_state, &old, v, false, __ATOMIC_RELAXED,
                                          __ATOMIC_RELAXED));
    if (v % loss_one_in)
        return false;
    COUNT(lost);
    return true;
}

void tcp_set_loss(uint32_t one_in)
{
    loss_one_in = one_in >= 2 ? one_in : 0;
}

/* (65e-26) n bytes of the send ring from position at (counted from the
 * first byte after the SYN; the ring's size is a power of two, so the
 * counting may wrap) into out, in at most two pieces. It was a byte at a
 * time, each with a division by the size, here and in tcp_send: a Celeron
 * J1900 sent 15 MB/s with one core busy in the sending program. */
static void ring_read(const struct tcp *t, uint32_t at, uint8_t *out, uint32_t n)
{
    at %= t->send_size;
    uint32_t first = t->send_size - at < n ? t->send_size - at : n;
    memcpy(out, t->send_buf + at, first);
    memcpy(out + first, t->send_buf, n - first);
}

/* Send one segment: flags, sequence number, data (and the MSS option on a
 * SYN). Under the lock. */
static void segment(struct tcp *t, uint8_t flags, uint32_t seq, const uint8_t *data, size_t n)
{
    if (flags & ACK) {
        t->ack_owed = false;                /* (65e) this one carries it */
        t->full_owed = 0;                   /* (65e-29) and what was held */
        t->ack_soon = false;
        t->delack_at = 0;
    }
    if (!t->own_mss)
        t->own_mss = inet_mss(&t->remote);
    uint8_t s[20 + 40 + MSS];
    bool ws = (flags & SYN) && t->ws_offer;         /* (65e) window scaling offered */
    bool sack = (flags & SYN) && t->sack_offer;     /* (65e) SACK offered */
    int blocks = !(flags & SYN) && (flags & ACK) && n == 0 && t->sack_ok ?
                 (t->gaps < SACK_BLOCKS ? t->gaps : SACK_BLOCKS) : 0;
    size_t header = flags & SYN ? 24 + (ws ? 4 : 0) + (sack ? 4 : 0) :
                    blocks ? 24 + 8 * (size_t)blocks : 20;
    put16(s, t->local_port);
    put16(s + 2, t->remote_port);
    put32(s + 4, seq);
    put32(s + 8, flags & ACK ? t->rcv_nxt : 0);
    s[12] = (uint8_t)(header / 4 << 4);
    s[13] = flags;
    uint32_t window = t->recv_size - t->recv_len;
    if (!(flags & SYN))
        window >>= t->rcv_shift;                    /* (65e: never in a SYN) */
    if (window > 65535)
        window = 65535;
    put16(s + 14, (uint16_t)window);

    put16(s + 16, 0);
    put16(s + 18, 0);
    if (flags & SYN) {
        s[20] = 2, s[21] = 4;                       /* option: our MSS */
        put16(s + 22, (uint16_t)t->own_mss);
        uint8_t *o = s + 24;
        if (ws)
            o[0] = 1, o[1] = 3, o[2] = 3, o[3] = RCV_SHIFT, o += 4;  /* (65e) NOP, window scale */
        if (sack)
            o[0] = 1, o[1] = 1, o[2] = 4, o[3] = 2;                  /* NOP, NOP, SACK permitted */
    } else if (blocks) {
        s[20] = 1, s[21] = 1, s[22] = 5, s[23] = (uint8_t)(2 + 8 * blocks);  /* (65e) SACK */
        for (int i = 0; i < blocks; i++) {
            put32(s + 24 + 8 * i, t->gap_start[i]);
            put32(s + 28 + 8 * i, t->gap_end[i]);
        }
    }
    memcpy(s + header, data, n);
    struct inet_addr src = t->local;
    if (inet_none(&src))
        inet_source(&t->remote, &src);
    put16(s + 16, ip_checksum_fold(ip_checksum_add(inet_pseudo(&src, &t->remote, IP_PROTO_TCP,
                                                               header + n), s, header + n)));
    COUNT(sent);
    if (lose())
        return;
    struct outbox *b = t->out ? t->out : &t->box;
    if (!b->ring && (b->ring = kmalloc(OUTBOX * sizeof(*b->ring))))
        b->size = OUTBOX;
    if (!b->ring || b->count == b->size) {
        inet_send(&src, &t->remote, IP_PROTO_TCP, s, header + n);    /* (full: at once) */
        return;
    }
    struct outgoing *o = &b->ring[(b->head + b->count++) % b->size];
    o->src = src;
    o->dst = t->remote;
    o->length = (uint16_t)(header + n);
    memcpy(o->bytes, s, header + n);
}

/* A reset for a segment that belongs to no connection, into the reply's
 * box (sent once the lock is free). */
static void reset_for(const struct inet_addr *src, const struct inet_addr *dst, const uint8_t *p,
                      size_t data_length, struct outbox *reply)
{
    uint8_t flags = p[13];
    if (flags & RST)
        return;
    struct tcp t = { .local = *dst, .remote = *src, .local_port = get16(p + 2),
                     .remote_port = get16(p), .recv_len = RECV_BUFFER, .recv_size = RECV_BUFFER,
                     .out = reply };
    if (flags & ACK) {
        segment(&t, RST, get32(p + 8), 0, 0);
    } else {
        t.rcv_nxt = get32(p + 4) + (uint32_t)data_length + (flags & SYN ? 1 : 0) +
                    (flags & FIN ? 1 : 0);
        segment(&t, RST | ACK, 0, 0, 0);
    }
}

/* snd_nxt moved forward: maybe beyond everything sent before. */
static void sent_up_to(struct tcp *t)
{
    if (before(t->snd_max, t->snd_nxt))
        t->snd_max = t->snd_nxt;
}

static void arm_timer(struct tcp *t)
{
    t->rto_at = now_ms() + t->rto;
}

static uint32_t mss_of(struct tcp *t);

/* Send what the window allows: new data from the send ring, then the FIN
 * once everything is out. Under the lock. */
static void output(struct tcp *t)
{
    if (t->state != ESTABLISHED && t->state != CLOSE_WAIT && t->state != FIN_WAIT_1 &&
        t->state != CLOSING && t->state != LAST_ACK)
        return;
    uint32_t mss = mss_of(t);
    for (;;) {
        uint32_t in_flight = t->snd_nxt - t->snd_una - (t->fin_sent ? 1 : 0);
        uint32_t window = t->snd_wnd < t->cwnd ? t->snd_wnd : t->cwnd;
        uint32_t waiting = t->send_len - in_flight;
        if (waiting == 0 || in_flight >= window)
            break;
        uint32_t n = waiting < mss ? waiting : mss;
        if (n > window - in_flight)
            n = window - in_flight;
        uint8_t data[MSS];
        ring_read(t, t->snd_una + in_flight - (t->iss + 1), data, n);
        /* Everything about the segment is recorded before it goes out (its
         * acknowledgement may come before segment() returns). */
        uint32_t seq = t->snd_nxt;
        if (!t->rtt_start && !before(seq, t->snd_max)) {   /* Karn: only new data is timed */
            t->rtt_start = arch_counter();
            t->rtt_seq = seq;
        }
        t->snd_nxt += n;
        sent_up_to(t);
        if (!t->rto_at)
            arm_timer(t);
        segment(t, ACK | PSH, seq, data, n);
    }
    if (t->fin_queued && !t->fin_sent && t->snd_nxt - t->snd_una == t->send_len) {
        uint32_t seq = t->snd_nxt;
        t->snd_nxt++;
        sent_up_to(t);
        t->fin_sent = t->fin_out = true;
        t->fin_seq = seq;
        if (!t->rto_at)
            arm_timer(t);
        segment(t, FIN | ACK, seq, 0, 0);
    }
    if (t->snd_wnd == 0 && t->send_len > 0 && !t->rto_at)
        arm_timer(t);                               /* the persist timer */
}

/* Send n bytes from seq again (fast retransmit), or the FIN if that is
 * what is outstanding. Under the lock. (65e-35) Only what went out before
 * (up to snd_max, the FIN not counted): a whole MSS asked for while less
 * was outstanding sent bytes snd_max never counted; the receiver took them,
 * and its ACKs, beyond snd_max now, were thrown away as acknowledging what
 * we never sent (RFC 5961's check) along with the window they opened:
 * both ends waited for ever (smpstress's tcp part found it, with the other
 * parts loading the cores). */
static void send_again(struct tcp *t, uint32_t seq, uint32_t n)
{
    uint32_t have = t->send_len - (seq - t->snd_una);
    uint32_t data_end = t->fin_out ? t->fin_seq : t->snd_max;
    uint32_t sent = before(seq, data_end) ? data_end - seq : 0;
    if (have > sent)
        have = sent;
    if (n > have)
        n = have;
    if (n == 0) {
        if (t->fin_sent)
            segment(t, FIN | ACK, seq, 0, 0);
        return;
    }
    uint8_t data[MSS];
    ring_read(t, seq - (t->iss + 1), data, n);
    segment(t, ACK | PSH, seq, data, n);
}

static uint32_t mss_of(struct tcp *t)
{
    uint32_t own = t->own_mss ? t->own_mss : MSS;
    return t->peer_mss < own ? t->peer_mss : own;
}

/* Send again from snd_una: the timer ran out. Under the lock. */
static void retransmit(struct tcp *t)
{
    bool handshake = t->state == SYN_SENT || t->state == SYN_RECEIVED;
    if (!handshake && t->snd_wnd == 0 && t->send_len > 0) {
        /* The window is closed: ask with one byte whether it opened (the
         * answer carries the window), backing off; no giving up while the
         * peer answers (its acknowledgements set retries back). */
        COUNT(probes);
        if (++t->retries > RETRIES * 2) {
            t->error = ERR_TIMEDOUT;
            t->state = CLOSED;
            wait_queue_wake_all(&t->waiters);
            return;
        }
        t->rto = t->rto * 2 > RTO_MAX ? RTO_MAX : t->rto * 2;
        t->rtt_start = 0;
        if (t->snd_nxt == t->snd_una) {
            t->snd_nxt++;                           /* the byte is outstanding now */
            sent_up_to(t);
        }
        arm_timer(t);
        send_again(t, t->snd_una, 1);
        return;
    }
    if (!handshake && t->snd_una == t->snd_max) {
        t->rto_at = 0;                              /* nothing outstanding after all */
        return;
    }
    COUNT(timeouts);
    if (++t->retries > RETRIES) {
        t->error = ERR_TIMEDOUT;
        t->state = CLOSED;
        wait_queue_wake_all(&t->waiters);
        return;
    }
    t->rto = t->rto * 2 > RTO_MAX ? RTO_MAX : t->rto * 2;     /* back off */
    t->rtt_start = 0;                               /* Karn: no sample from a resent one */
    t->ssthresh = (t->snd_max - t->snd_una) / 2 > 2 * MSS ? (t->snd_max - t->snd_una) / 2
                                                         : 2 * MSS;
    t->cwnd = MSS;
    t->recovering = false;
    t->dupacks = 0;
    t->scored = 0;                                  /* (65e) start over from snd_una */
    if (t->state == SYN_SENT) {
        arm_timer(t);
        segment(t, SYN, t->iss, 0, 0);
        return;
    } else if (t->state == SYN_RECEIVED) {
        arm_timer(t);
        segment(t, SYN | ACK, t->iss, 0, 0);
        return;
    } else {
        /* Go back to the oldest unacknowledged byte and send from there. */
        t->snd_nxt = t->snd_una;
        t->fin_sent = false;
        arm_timer(t);
        output(t);
        return;
    }
}

/* Free the slot (the table's lock and t's held); false while its outbox
 * still sends (the tcp thread frees it later). The lock stays, every other
 * field is cleared: give the lock back before the table's. */
static bool free_slot(struct tcp *t)
{
    if (t->box.count || t->box.sending)
        return false;
    struct tcp *l = t->listener;                    /* not accepted yet: out of the queue */
    if (l) {
        for (int i = 0; i < l->backlog_count; i++) {
            if (l->backlog[i] == t) {
                memmove(l->backlog + i, l->backlog + i + 1,
                        (size_t)(l->backlog_count - i - 1) * sizeof(l->backlog[0]));
                l->backlog_count--;
                break;
            }
        }
    }
    kfree(t->send_buf);
    kfree(t->recv_buf);
    kfree(t->box.ring);
    wait_queue_detach(&t->waiters);         /* (65e) no poller left on it */
    memset((uint8_t *)t + offsetof(struct tcp, state), 0, sizeof(*t) - offsetof(struct tcp, state));
    return true;
}

/* A dead slot: one the program closed, or a half-open connection that
 * never finished and was never accepted (a SYN flood otherwise fills all
 * slots until a reboot: 49a). */
static bool dead(const struct tcp *t)
{
    return t->state == CLOSED && (t->user_closed || t->listener);
}

/* t freed if it is dead (no lock held). */
static void free_if_dead(struct tcp *t)
{
    mutex_lock(&table_lock);
    mutex_lock(&t->lock);
    if (t->used && dead(t))
        free_slot(t);
    mutex_unlock(&t->lock);
    mutex_unlock(&table_lock);
}

/* The tcp thread: timers. Every TICK_MS, and (65e-29) when a held ACK is
 * due. */
static void timers(void *context)
{
    (void)context;
    for (;;) {
        uint64_t until = __atomic_load_n(&timer_wake_ms, __ATOMIC_RELAXED) / (1000 / TIMER_HZ);
        if (until > timer_ticks())                  /* (0 would be no deadline at all) */
            wait_event_until(&timer_wq, __atomic_load_n(&timer_kicked, __ATOMIC_RELAXED), until);
        __atomic_store_n(&timer_kicked, false, __ATOMIC_RELAXED);
        uint64_t now = now_ms(), wake = now + TICK_MS;
        for (int i = 0; i < CONNECTIONS; i++) {
            struct tcp *t = &table[i];
            mutex_lock(&table_lock);
            if (!t->used) {
                mutex_unlock(&table_lock);
                continue;
            }
            mutex_lock(&t->lock);
            if (dead(t) && free_slot(t)) {
                mutex_unlock(&t->lock);
                mutex_unlock(&table_lock);
                continue;
            }
            mutex_unlock(&table_lock);
            if (t->delack_at && now >= t->delack_at) {
                if (t->full_owed && t->state != CLOSED)
                    segment(t, ACK, t->snd_max, 0, 0);    /* (65e-29) no second one came */
                t->delack_at = 0;
            } else if (t->delack_at && t->delack_at < wake) {
                wake = t->delack_at;
            }
            if ((t->state == TIME_WAIT || t->state == FIN_WAIT_2) && t->time_wait_until &&
                now_ms() >= t->time_wait_until)
                t->state = CLOSED;                  /* TIME_WAIT done, or a peer that never FINs */
            else if (t->rto_at && now_ms() >= t->rto_at)
                retransmit(t);
            release(t);                             /* (dead now: freed at the next look) */
        }
        __atomic_store_n(&timer_wake_ms, wake, __ATOMIC_RELAXED);
    }
}

/* A free slot with its buffers; 0 if there is none. Under the table's lock;
 * nobody holds a free slot's own lock. */
static struct tcp *new_connection(void)
{
    if (!thread_started)
        thread_started = thread_create("tcp", timers, 0) != 0;
    for (int i = 0; i < CONNECTIONS; i++) {
        struct tcp *t = &table[i];
        if (t->used)
            continue;
        if (!t->lock.name)
            t->lock = (struct mutex)MUTEX_INIT("tcp connection");
        memset((uint8_t *)t + offsetof(struct tcp, state), 0, sizeof(*t) - offsetof(struct tcp, state));
        t->send_buf = kmalloc(BUFFER);
        t->send_size = BUFFER;
        t->recv_buf = kmalloc(RECV_BUFFER);
        t->recv_size = RECV_BUFFER;
        if (!t->send_buf || !t->recv_buf) {
            kfree(t->send_buf);
            kfree(t->recv_buf);
            return 0;
        }
        t->used = true;
        t->waiters.pollable = true;
        t->rto = RTO_INITIAL;
        t->peer_mss = 536;                          /* until the peer says more */
        t->cwnd = 2 * MSS;
        t->ssthresh = SEND_BIG;     /* (65e) "arbitrarily high" (RFC 5681); was 64 KiB, past which
                                     * the window grew one segment per round trip */
        random_bytes(&t->iss, sizeof(t->iss));
        return t;
    }
    return 0;
}

/* (65e) A connection that may scale its window: its receive buffer starts
 * at RECV_START (nothing came yet) and grows from there. */
static void recv_start(struct tcp *t)
{
    uint8_t *start = kmalloc(RECV_START);
    if (start) {
        kfree(t->recv_buf);
        t->recv_buf = start;
        t->recv_size = RECV_START;
    }
}

static bool port_taken(uint16_t port)
{
    for (int i = 0; i < CONNECTIONS; i++)
        if (table[i].used && table[i].local_port == port && table[i].state == LISTEN)
            return true;
    return false;
}

static uint16_t ephemeral_port(void)
{
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = next_port;
        next_port = next_port == 65535 ? 49152 : next_port + 1;
        bool taken = false;
        for (int i = 0; i < CONNECTIONS && !taken; i++)
            taken = table[i].used && table[i].local_port == p;
        if (!taken)
            return p;
    }
    return 0;
}

/* --- for sockets --------------------------------------------------------------- */

void tcp_batch_end(void)
{
    struct thread *me = thread_current();
    for (int i = 0; i < CONNECTIONS; i++) {
        struct tcp *t = &table[i];
        /* (65e-35) A look without the locks first: only this thread's own
         * marks matter, and it made them. */
        if (!__atomic_load_n(&t->ack_owed, __ATOMIC_RELAXED) ||
            __atomic_load_n(&t->owed_by, __ATOMIC_RELAXED) != me)
            continue;
        mutex_lock(&table_lock);
        if (!t->used) {
            mutex_unlock(&table_lock);
            continue;
        }
        take(t);
        if (t->ack_owed && t->owed_by == me) {
            if (!t->ack_soon && t->full_owed == 1) {
                /* (65e-29) One full segment: its ACK waits for the next (RFC
                 * 1122, 5681: at least every second full one), at most
                 * DELACK_MS. A receiver acknowledging every burst its card
                 * brought sent an ACK per 1.5 segments of a Wi-Fi upload, each
                 * the access point's own turn on the air. */
                t->ack_owed = false;
                if (!t->delack_at) {
                    t->delack_at = now_ms() + DELACK_MS;
                    COUNT(delayed);
                    if (t->delack_at < __atomic_load_n(&timer_wake_ms, __ATOMIC_RELAXED)) {
                        __atomic_store_n(&timer_kicked, true, __ATOMIC_RELAXED);
                        wait_queue_wake_all(&timer_wq);
                    }
                }
            } else {
                segment(t, ACK, t->snd_max, 0, 0);
            }
        }
        release(t);
    }
}

int tcp_poll(struct tcp *t)
{
    poll_watch(&t->waiters);                /* (65e) before the state is read */
    mutex_lock(&table_lock);
    if (t->state == LISTEN) {               /* (65e-35: set and left under both locks) */
        int r = t->backlog_count > 0 ? POLLIN : 0;
        mutex_unlock(&table_lock);
        return r;
    }
    take(t);
    int r = 0;
    if (t->state == CLOSED)                         /* (reset or ended: reads give the end; 53b:
                                                     * a refused connect is POLLOUT | POLLERR) */
        r = POLLIN | POLLOUT | POLLHUP | (t->remote_port && t->error != 0 ? POLLERR : 0) |
            (t->remote_port && !t->error && !t->peer_fin ? POLLERR : 0);
    else {
        if (t->recv_len || t->peer_fin)
            r |= POLLIN;
        if ((t->state == ESTABLISHED || t->state == CLOSE_WAIT) && t->send_len < t->send_size)
            r |= POLLOUT;
    }
    release(t);
    return r;
}

int tcp_bind(struct tcp **out, uint16_t port, unsigned families)
{
    mutex_lock(&table_lock);
    int r = 0;
    if (*out) {
        r = ERR_INVAL;
    } else if (port && port_taken(port)) {
        r = ERR_ADDRINUSE;
    } else if (!(*out = new_connection())) {
        r = ERR_NOMEM;
    } else {
        (*out)->local_port = port ? port : ephemeral_port();
        (*out)->families = families;
    }
    mutex_unlock(&table_lock);
    return r;
}

int tcp_listen(struct tcp **out, int backlog, unsigned families)
{
    int r = *out ? 0 : tcp_bind(out, 0, families);
    if (r)
        return r;
    mutex_lock(&table_lock);
    struct tcp *t = *out;
    mutex_lock(&t->lock);                           /* (LISTEN is set under both) */
    if (t->state != CLOSED) {
        r = ERR_INVAL;
    } else {
        t->state = LISTEN;
        t->backlog_max = backlog < 1 ? 1 : backlog > BACKLOG_MAX ? BACKLOG_MAX : backlog;
    }
    mutex_unlock(&t->lock);
    mutex_unlock(&table_lock);
    return r;
}

int tcp_accept(struct tcp *t, struct tcp **out)
{
    mutex_lock(&table_lock);                        /* (the backlog is the table's) */
    if (t->state != LISTEN) {
        mutex_unlock(&table_lock);
        return ERR_INVAL;
    }
    bool none = t->backlog_count == 0;
    mutex_unlock(&table_lock);
    if (none && thread_current()->dont_wait)
        return ERR_AGAIN;                           /* (53b) */
    if (!wait_event_interruptible(&t->waiters, t->backlog_count > 0))
        return ERR_INTR;
    mutex_lock(&table_lock);
    if (t->backlog_count == 0) {                    /* reset meanwhile */
        mutex_unlock(&table_lock);
        return tcp_accept(t, out);
    }
    struct tcp *c = t->backlog[0];
    mutex_lock(&c->lock);
    c->listener = 0;                                /* the program's now */
    mutex_unlock(&c->lock);
    *out = c;
    memmove(t->backlog, t->backlog + 1, (size_t)--t->backlog_count * sizeof(t->backlog[0]));
    mutex_unlock(&table_lock);
    return 0;
}

int tcp_connect(struct tcp **out, const struct inet_addr *ip, uint16_t port, unsigned families)
{
    if (!(families & inet_family(ip)))
        return ERR_AFNOSUPPORT;                     /* not a family the socket takes */
    int r = *out ? 0 : tcp_bind(out, 0, families);
    if (r)
        return r;
    mutex_lock(&table_lock);                        /* (the peer is find()'s: set under both) */
    struct tcp *t = *out;
    mutex_lock(&t->lock);
    if (t->state != CLOSED) {                       /* (53b: a connect started before) */
        int r = t->state == SYN_SENT ? ERR_ALREADY : t->state == LISTEN ? ERR_INVAL : ERR_ISCONN;
        mutex_unlock(&t->lock);
        mutex_unlock(&table_lock);
        return r;
    }
    if (t->remote_port) {                           /* (ended: it was refused or reset) */
        int r = t->error ? t->error : ERR_CONNREFUSED;
        mutex_unlock(&t->lock);
        mutex_unlock(&table_lock);
        return r;
    }
    t->remote = *ip;
    t->remote_port = port;
    if (!inet_source(ip, &t->local) && !inet_is_v4(ip)) {
        t->remote_port = 0;
        mutex_unlock(&t->lock);
        mutex_unlock(&table_lock);
        return ERR_NETUNREACH;                      /* (IPv6: no route; IPv4 as before) */
    }
    mutex_unlock(&table_lock);
    t->own_mss = inet_mss(ip);                      /* the card its route takes */
    t->ws_offer = t->sack_offer = true;             /* (65e) if the peer agrees */
    recv_start(t);
    t->snd_una = t->iss;
    t->snd_nxt = t->snd_max = t->iss + 1;
    t->state = SYN_SENT;
    t->rtt_start = arch_counter();
    t->rtt_seq = t->iss;
    arm_timer(t);
    segment(t, SYN, t->iss, 0, 0);
    release(t);
    if (thread_current()->dont_wait)
        return ERR_INPROGRESS;                      /* (53b: poll for POLLOUT, then SO_ERROR) */
    if (!wait_event_interruptible(&t->waiters, t->state != SYN_SENT))
        return ERR_INTR;
    return t->state == ESTABLISHED || t->state == CLOSE_WAIT ? 0 : t->error ? t->error
                                                                         : ERR_CONNREFUSED;
}

/* (65e) The program has more to send and the buffer is full: if the peer's
 * window and our congestion window would carry more than it holds (the
 * window scaled; the congestion window grown to 3/4 of the buffer, capped
 * at it), the buffer holds the upload back: double it, up to SEND_BIG (as
 * Linux sizes its from the congestion window). The ring is laid out by
 * sequence number: what waits is copied to its place in the larger one.
 * Without memory for it the buffer stays. Under the lock. */
static void send_grow(struct tcp *t)
{
    if (!t->snd_shift || t->send_size >= SEND_BIG || t->snd_wnd < t->send_size ||
        t->cwnd < t->send_size - t->send_size / 4)
        return;
    uint32_t size = t->send_size * 2;
    uint8_t *buf = kmalloc(size);
    if (!buf)
        return;
    for (uint32_t i = 0; i < t->send_len; i++) {
        uint32_t seq = t->snd_una + i - (t->iss + 1);
        buf[seq % size] = t->send_buf[seq % t->send_size];
    }
    kfree(t->send_buf);
    t->send_buf = buf;
    t->send_size = size;
    COUNT(sgrown);
}

int64_t tcp_send(struct tcp *t, const void *data, size_t length, uint32_t timeout_ms)
{
    const uint8_t *p = data;
    size_t done = 0;
    uint64_t deadline = timeout_ms ? timer_ticks() + ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000
                                   : 0;
    while (done < length) {
        if (timeout_ms) {                           /* (53a: SO_SNDTIMEO) */
            if (!wait_event_until(&t->waiters, (t->state != ESTABLISHED && t->state != CLOSE_WAIT) ||
                                  t->send_len < t->send_size || thread_current()->stop_requested,
                                  deadline))
                return done ? (int64_t)done : -ERR_AGAIN;
            if (thread_current()->stop_requested)
                return done ? (int64_t)done : -ERR_INTR;
        } else if (t->send_len >= t->send_size && thread_current()->dont_wait) {
            return done ? (int64_t)done : -ERR_AGAIN;   /* (53b) */
        } else if (!wait_event_interruptible(&t->waiters, (t->state != ESTABLISHED &&
                                                           t->state != CLOSE_WAIT) ||
                                                          t->send_len < t->send_size)) {
            return done ? (int64_t)done : -ERR_INTR;
        }
        mutex_lock(&t->lock);
        if (t->state != ESTABLISHED && t->state != CLOSE_WAIT) {
            int error = t->error ? t->error : ERR_PIPE;
            release(t);
            return done ? (int64_t)done : -error;
        }
        size_t n = length - done < t->send_size - t->send_len ? length - done
                                                              : t->send_size - t->send_len;
        if (n) {                                    /* (65e-26) in at most two pieces */
            uint32_t at = (t->snd_una + t->send_len - (t->iss + 1)) % t->send_size;
            size_t first = t->send_size - at < n ? t->send_size - at : n;
            memcpy(t->send_buf + at, p + done, first);
            memcpy(t->send_buf, p + done + first, n - first);
            done += n;
            t->send_len += (uint32_t)n;
        }
        if (done < length)
            send_grow(t);                           /* (65e) full: maybe the buffer holds it back */
        output(t);
        release(t);
    }
    return (int64_t)done;
}

int64_t tcp_receive(struct tcp *t, void *data, size_t size, uint32_t timeout_ms)
{
    uint64_t deadline = timeout_ms ? timer_ticks() + ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000
                                   : 0;
    for (;;) {
        mutex_lock(&t->lock);
        if (t->recv_len) {
            size_t n = t->recv_len < size ? t->recv_len : size;
            uint8_t *p = data;
            size_t first = t->recv_size - t->recv_head < n ? t->recv_size - t->recv_head : n;   /* (65e) */
            memcpy(p, t->recv_buf + t->recv_head, first);
            memcpy(p + first, t->recv_buf, n - first);
            bool was_full = t->recv_size - t->recv_len < MSS;
            t->recv_head = (t->recv_head + (uint32_t)n) % t->recv_size;
            t->recv_len -= (uint32_t)n;
            if (was_full && t->recv_size - t->recv_len >= MSS)
                segment(t, ACK, t->snd_max, 0, 0);   /* the window opened again */
            release(t);
            return (int64_t)n;
        }
        if (t->peer_fin || t->state == CLOSED || t->read_shut) {
            int error = t->peer_fin || t->read_shut ? 0 : t->error;
            release(t);
            return error ? -error : 0;              /* 0: the end of the stream */
        }
        release(t);
        if (thread_current()->dont_wait)
            return -ERR_AGAIN;                      /* (53b) */
        bool ready = timeout_ms
            ? wait_event_until(&t->waiters, t->recv_len || t->peer_fin || t->state == CLOSED ||
                               thread_current()->stop_requested, deadline)
            : wait_event_interruptible(&t->waiters, t->recv_len || t->peer_fin ||
                                                    t->state == CLOSED);
        if (thread_current()->stop_requested)
            return -ERR_INTR;
        if (!ready && timeout_ms)
            return -ERR_TIMEDOUT;
    }
}

struct inet_addr tcp_peer(struct tcp *t, uint16_t *port)
{
    if (port)
        *port = t ? t->remote_port : 0;
    return t ? t->remote : inet_v4(0);
}

uint16_t tcp_local_port(struct tcp *t)
{
    return t ? t->local_port : 0;
}

int tcp_error(struct tcp *t)
{
    if (!t)
        return 0;
    mutex_lock(&t->lock);
    int e = t->state == CLOSED && t->remote_port ? (t->error ? t->error : ERR_CONNREFUSED) : 0;
    release(t);
    return e;
}

struct inet_addr tcp_local_address(struct tcp *t)
{
    return t ? t->local : inet_v4(0);
}

void tcp_path_mtu(const struct inet_addr *local, const struct inet_addr *remote, uint16_t local_port,
                  uint16_t remote_port, uint32_t mtu)
{
    uint32_t mss = mtu > 60 ? mtu - 60 : 0;         /* (IPv6's 40 bytes and TCP's 20) */
    if (mss < 1220)
        mss = 1220;                                 /* IPv6's minimum MTU, 1280, less them */
    mutex_lock(&table_lock);
    for (int i = 0; i < CONNECTIONS; i++) {
        struct tcp *t = &table[i];
        if (!t->used || t->local_port != local_port || t->remote_port != remote_port ||
            !inet_same(&t->local, local) || !inet_same(&t->remote, remote))
            continue;
        mutex_lock(&t->lock);
        if (!t->own_mss || mss < t->own_mss) {
            t->own_mss = mss;
            COUNT(too_big);
            if (t->snd_una != t->snd_max) {         /* what was lost goes again, smaller */
                t->snd_nxt = t->snd_una;
                t->fin_sent = false;
                output(t);
            }
        }
        release(t);                                 /* (rare: sent with the table's lock held) */
    }
    mutex_unlock(&table_lock);
}

/* (53a) shutdown: the sending side ends with a FIN (the connection reads
 * on); the receiving side reads the end from now on. */
void tcp_shutdown(struct tcp *t, bool read, bool write)
{
    mutex_lock(&t->lock);
    if (read)
        t->read_shut = true;
    if (write && !t->fin_queued) {
        if (t->state == ESTABLISHED || t->state == SYN_RECEIVED) {
            t->fin_queued = true;
            t->state = FIN_WAIT_1;
            output(t);
        } else if (t->state == CLOSE_WAIT) {
            t->fin_queued = true;
            t->state = LAST_ACK;
            output(t);
        }
    }
    release(t);
    wait_queue_wake_all(&t->waiters);
}

void tcp_print(void)
{
    static const char *const names[] = { "closed", "listening", "SYN sent", "SYN received",
                                         "established", "FIN wait 1", "FIN wait 2",
                                         "close wait", "closing", "last ACK", "time wait" };
    mutex_lock(&table_lock);                        /* (the states as they are: a look) */
    int open = 0, count[TIME_WAIT + 1] = { 0 };
    for (int i = 0; i < CONNECTIONS; i++) {
        open += table[i].used;
        if (table[i].used)
            count[table[i].state]++;
    }
    char states[160] = "";
    for (int st = 0; st <= TIME_WAIT; st++) {
        size_t at = strlen(states);
        if (count[st])
            ksnprintf(states + at, sizeof(states) - at, "%s%d %s", at ? ", " : " (", count[st],
                      names[st]);
    }
    if (states[0])
        ksnprintf(states + strlen(states), sizeof(states) - strlen(states), ")");
    char lost[80] = "";
    if (loss_one_in)
        ksnprintf(lost, sizeof(lost), ", %lu lost on purpose (1 in %u)", stats.lost, loss_one_in);
    if (stats.too_big)
        ksnprintf(lost + strlen(lost), sizeof(lost) - strlen(lost), ", %lu smaller (too big)",
                  stats.too_big);
    kprintf("tcp: %d connection(s)%s; segments %lu sent, %lu received; sent again: %lu after a "
            "timeout, %lu fast; %lu kept after a gap, %lu window probes; %lu with scaled windows, "
            "%lu with SACK, receive buffers doubled %lu times; %lu holes resent from SACK, send "
            "buffers doubled %lu times, %lu ACKs delayed, %lu reset with data unread%s\n", open,
            states, stats.sent, stats.received, stats.timeouts, stats.fast, stats.kept,
            stats.probes, stats.scaled, stats.sacked, stats.grown, stats.holes, stats.sgrown,
            stats.delayed, stats.resets_unread, lost);
    mutex_unlock(&table_lock);
}

void tcp_close(struct tcp *t)
{
    mutex_lock(&table_lock);
    mutex_lock(&t->lock);
    t->user_closed = true;
    /* (36e-8) Closed with received data unread: a RST, not a FIN (RFC
     * 1122 4.2.2.13, as Linux and the BSDs): the data is lost and the
     * peer is told so at once. With a FIN its writes waited on our zero
     * window until FIN_WAIT_2's limit freed us (a server ~6 s on each
     * next client). The RST goes out in release(); the tcp thread frees
     * the closed connection. */
    bool reset = (t->recv_len || t->gaps) &&
                 (t->state == SYN_RECEIVED || t->state == ESTABLISHED || t->state == CLOSE_WAIT);
    if (reset) {
        segment(t, RST | ACK, t->snd_max, 0, 0);
        t->state = CLOSED;
        COUNT(resets_unread);
    }
    switch (reset ? CLOSED : t->state) {
    case LISTEN:
        /* Connections still waiting to be accepted (or in their handshake)
         * end too. */
        for (int i = 0; i < CONNECTIONS; i++) {
            struct tcp *c = &table[i];
            if (c->used && c->listener == t) {
                mutex_lock(&c->lock);               /* (a listener's, then its own: never back) */
                segment(c, RST | ACK, c->snd_max, 0, 0);
                c->state = CLOSED;
                c->user_closed = true;
                c->listener = 0;
                release(c);                         /* (freed by the tcp thread) */
            }
        }
        t->backlog_count = 0;
        t->state = CLOSED;
        break;
    case SYN_SENT:
        t->state = CLOSED;
        break;
    case SYN_RECEIVED:
    case ESTABLISHED:
        t->fin_queued = true;
        t->state = FIN_WAIT_1;
        output(t);
        break;
    case CLOSE_WAIT:
        t->fin_queued = true;
        t->state = LAST_ACK;
        output(t);
        break;
    default:
        break;
    }
    if (t->state == CLOSED && !reset && free_slot(t)) {
        mutex_unlock(&t->lock);                     /* (before the table's: the slot is free) */
        mutex_unlock(&table_lock);
        return;
    }
    mutex_unlock(&table_lock);
    release(t);                                     /* (closed later: freed by the tcp thread) */
}

/* --- arriving segments ------------------------------------------------------------ */

static struct tcp *find(const struct inet_addr *src, uint16_t src_port, uint16_t dst_port)
{
    struct tcp *listener = 0;
    for (int i = 0; i < CONNECTIONS; i++) {
        struct tcp *t = &table[i];
        if (!t->used || t->local_port != dst_port)
            continue;
        if (t->state == LISTEN)
            listener = t;
        else if (t->state != CLOSED && inet_same(&t->remote, src) && t->remote_port == src_port)
            return t;
    }
    return listener;
}

/* The peer's MSS option of a SYN. At least 64: a peer that sends 0 (or a
 * tiny value) must not stall output (n = min(waiting, mss) would be 0, an
 * endless loop under the lock) or make the congestion window reach 0 (a
 * division by zero in the slow-start-exit path); RFC 1122 and Linux use a
 * similar floor. */
#define MSS_FLOOR 64
static uint32_t mss_option(const uint8_t *p, size_t header)
{
    uint32_t mss = 536;
    for (size_t i = 20; i + 1 < header;) {
        if (p[i] == 0)
            break;
        if (p[i] == 1) {
            i++;
            continue;
        }
        if (p[i] == 2 && p[i + 1] == 4 && i + 4 <= header) {
            mss = get16(p + i + 2);
            break;
        }
        if (p[i + 1] < 2)
            break;
        i += p[i + 1];
    }
    return mss < MSS_FLOOR ? MSS_FLOOR : mss;
}

/* (65e) Where option kind (of length) is in a segment's options, or -1. */
static int find_option(const uint8_t *p, size_t header, uint8_t kind, uint8_t length)
{
    for (size_t i = 20; i + 1 < header;) {
        if (p[i] == 0)
            break;
        if (p[i] == 1) {
            i++;
            continue;
        }
        if (p[i] == kind && p[i + 1] == length && i + length <= header)
            return (int)i;
        if (p[i + 1] < 2)
            break;
        i += p[i + 1];
    }
    return -1;
}

/* (65e) Where option kind is (whatever its length), or -1. */
static int find_option_any(const uint8_t *p, size_t header, uint8_t kind)
{
    for (size_t i = 20; i + 1 < header;) {
        if (p[i] == 0)
            break;
        if (p[i] == 1) {
            i++;
            continue;
        }
        if (p[i] == kind && p[i + 1] >= 2 && i + p[i + 1] <= header)
            return (int)i;
        if (p[i + 1] < 2)
            break;
        i += p[i + 1];
    }
    return -1;
}

/* (65e) The peer's window scale option (RFC 7323) of a SYN: its shift, or
 * -1 without one. */
static int wscale_option(const uint8_t *p, size_t header)
{
    int at = find_option(p, header, 3, 3);
    return at < 0 ? -1 : p[at + 2];
}

/* --- SYN cookies (49f) ----------------------------------------------------------
 *
 * A listener keeps no state for a SYN: it answers with a SYN-ACK whose
 * sequence number is a cookie that encodes the peer's MSS (coarsely, from a
 * small table) and a keyed hash of the four-tuple and a coarse time, and
 * builds the connection only when a valid ACK returns (its ack = cookie + 1).
 * A flood of spoofed SYNs then costs no memory and no table slot, and a
 * blind attacker cannot forge a cookie without the 64-bit secret. The exact
 * peer MSS is lost (the table value just below it is used). The mixing is
 * our own; it is not a cryptographic MAC, but the secret is unknown to the
 * attacker and a forgery must also hit the 24-bit hash (Linux's size).
 *
 * (65e-25) The bits: 31 the count's lowest (the hash tells it from the
 * one two before), 30-29 the MSS's index, 28 SACK permitted, 27-24 the
 * window scale (0 none, else the shift + 1), 23-0 the hash, over the
 * options too (a forged ACK cannot change them). It was 3 bits of count,
 * the index and 27 bits of hash. */
static const uint32_t cookie_mss[4] = { MSS_FLOOR, 536, 1200, MSS };
static uint64_t cookie_secret;
#define COOKIE_COUNT_MS 16000           /* a cookie is valid for this count and the one before */

static uint32_t cookie_hash(const struct inet_addr *src, const struct inet_addr *dst, uint16_t sport,
                            uint16_t dport, uint32_t count, uint32_t options)
{
    if (!cookie_secret) {
        uint64_t s;
        do {
            random_bytes(&s, sizeof(s));
        } while (!s);
        cookie_secret = s;
    }
    /* Both addresses, 8 bytes at a time (an IPv4 one's last 8: its
     * 0xFFFF and the address, as before 64c's 16 bytes). */
    uint64_t h = cookie_secret;
    for (int half = 0; half < 4; half++) {
        const uint8_t *b = (half < 2 ? src->b : dst->b) + (half & 1) * 8;
        uint64_t w = 0;
        for (int k = 0; k < 8; k++)
            w = w << 8 | b[k];
        h ^= w;
        h *= 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    h ^= ((uint64_t)sport << 48) | ((uint64_t)dport << 32) | count;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    h ^= options;
    h *= 0x9E3779B97F4A7C15ull;
    h ^= h >> 31;
    h ^= cookie_secret;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 29;
    return (uint32_t)h;
}

static uint32_t cookie_count(void)
{
    return (uint32_t)(now_ms() / COOKIE_COUNT_MS);
}

/* The SYN's offers in a cookie's bits 28-24: shift -1 is none. */
static uint32_t cookie_options(int shift, bool sack)
{
    return (sack ? 0x10u : 0) | (shift < 0 ? 0 : (uint32_t)(shift > SHIFT_MAX ? SHIFT_MAX : shift) + 1);
}

static uint32_t make_cookie(const struct inet_addr *src, const struct inet_addr *dst, uint16_t sport,
                            uint16_t dport, uint32_t mss, uint32_t options)
{
    uint32_t idx = 0;
    for (uint32_t i = 0; i < 4; i++)
        if (mss >= cookie_mss[i])
            idx = i;
    uint32_t count = cookie_count();
    return (count & 1) << 31 | idx << 29 | options << 24 |
           (cookie_hash(src, dst, sport, dport, count, idx << 5 | options) & 0x00FFFFFFu);
}

/* ack - 1 as a cookie we would have sent: the encoded peer MSS (and in
 * *options the SYN's offers), or 0 if it is not one of ours (within the
 * last two counts). */
static uint32_t check_cookie(const struct inet_addr *src, const struct inet_addr *dst, uint16_t sport,
                             uint16_t dport, uint32_t ack, uint32_t *options)
{
    uint32_t c = ack - 1;
    uint32_t low = c >> 31, idx = c >> 29 & 3, hash = c & 0x00FFFFFFu;
    *options = c >> 24 & 0x1F;
    uint32_t now = cookie_count();
    for (uint32_t d = 0; d <= 1; d++) {
        uint32_t count = now - d;
        if ((count & 1) == low &&
            (cookie_hash(src, dst, sport, dport, count, idx << 5 | *options) & 0x00FFFFFFu) == hash)
            return cookie_mss[idx];
    }
    return 0;
}

/* A round trip measured: the timeout from it (RFC 6298). */
static void sample(struct tcp *t)
{
    uint64_t hz = arch_counter_hz();
    uint32_t r = hz ? (uint32_t)((arch_counter() - t->rtt_start) * 1000 / hz) : RTO_INITIAL;
    if (!t->srtt) {
        t->srtt = r;
        t->rttvar = r / 2;
    } else {
        uint32_t diff = t->srtt > r ? t->srtt - r : r - t->srtt;
        t->rttvar = (3 * t->rttvar + diff) / 4;
        t->srtt = (7 * t->srtt + r) / 8;
    }
    uint32_t rto = t->srtt + 4 * t->rttvar;
    t->rto = rto < RTO_MIN ? RTO_MIN : rto > RTO_MAX ? RTO_MAX : rto;
    t->rtt_start = 0;
}

/* Data [start, end) arrived after a gap (it is in the ring already):
 * remember it, merged with the ranges it touches. Under the lock. */
/* (65e) Range i out, the others in their order (newest first). */
static void drop_gap(struct tcp *t, int i)
{
    t->gaps--;
    memmove(t->gap_start + i, t->gap_start + i + 1, (size_t)(t->gaps - i) * sizeof(t->gap_start[0]));
    memmove(t->gap_end + i, t->gap_end + i + 1, (size_t)(t->gaps - i) * sizeof(t->gap_end[0]));
}

static void keep_gap(struct tcp *t, uint32_t start, uint32_t end)
{
    COUNT(kept);
    for (int i = 0; i < t->gaps;) {
        if (!before(end, t->gap_start[i]) && !before(t->gap_end[i], start)) {
            if (before(t->gap_start[i], start))
                start = t->gap_start[i];
            if (before(end, t->gap_end[i]))
                end = t->gap_end[i];
            drop_gap(t, i);                         /* absorbed into the new one */
        } else {
            i++;
        }
    }
    /* No room for another range: its bytes stay in the ring but count as
     * not received (the sender sends them again). (65e) The new range goes
     * first: SACK reports the newest first (RFC 2018). */
    if (t->gaps < GAPS) {
        memmove(t->gap_start + 1, t->gap_start, (size_t)t->gaps * sizeof(t->gap_start[0]));
        memmove(t->gap_end + 1, t->gap_end, (size_t)t->gaps * sizeof(t->gap_end[0]));
        t->gap_start[0] = start;
        t->gap_end[0] = end;
        t->gaps++;
    }
}

/* (65e) The buffer doubles (up to RECV_BIG) when in one round trip more
 * than half of it came, as Linux grows its (tcp_rcv_space_adjust): then the
 * window, not the line or the program, holds the sender back; a program
 * that reads slowly shrinks the window and nothing grows. The round trip as
 * the receiver sees it: from announcing a window until that much has come,
 * the shortest such time (a sender the window holds back sends one window
 * per round trip). The ring is copied from its head, so every byte and kept
 * range stays at its offset; without memory for it the buffer stays. */
static void grow_buffer(struct tcp *t)
{
    uint32_t size = t->recv_size * 2;
    uint8_t *buf = kmalloc(size);
    if (!buf)
        return;
    uint32_t first = t->recv_size - t->recv_head;
    memcpy(buf, t->recv_buf + t->recv_head, first);
    memcpy(buf + first, t->recv_buf, t->recv_head);
    kfree(t->recv_buf);
    t->recv_buf = buf;
    t->recv_head = 0;
    t->recv_size = size;
    COUNT(grown);
}

/* After data came in order (rcv_nxt moved). */
static void grow_check(struct tcp *t)
{
    if (!t->rcv_shift || t->recv_size >= RECV_BIG)
        return;
    uint64_t now = arch_counter();
    if (!t->rtt_t0) {
        t->rtt_t0 = t->span_t0 = now;
        t->rtt_mark = t->rcv_nxt + (t->recv_size - t->recv_len);
        t->span_seq = t->rcv_nxt;
        return;
    }
    if (!before(t->rcv_nxt, t->rtt_mark)) {         /* a window's worth came: a sample */
        uint64_t sample = now - t->rtt_t0;
        if (!t->rcv_rtt || sample < t->rcv_rtt)
            t->rcv_rtt = sample;
        t->rtt_t0 = now;
        t->rtt_mark = t->rcv_nxt + (t->recv_size - t->recv_len);
    }
    if (t->rcv_rtt && now - t->span_t0 >= t->rcv_rtt) {
        uint32_t came = t->rcv_nxt - t->span_seq;
        t->span_t0 = now;
        t->span_seq = t->rcv_nxt;
        if (2 * (uint64_t)came > t->recv_size)
            grow_buffer(t);
    }
}

/* (65e) A segment's SACK blocks onto the scoreboard: kept sorted and
 * merged, nothing at or below snd_una, nothing past what we sent. */
static void score_add(struct tcp *t, const uint8_t *p, size_t header)
{
    int at = find_option_any(p, header, 5);
    if (at < 0)
        return;
    int length = p[at + 1];
    for (int k = at + 2; k + 8 <= at + length && (size_t)k + 8 <= header; k += 8) {
        uint32_t start = get32(p + k), end = get32(p + k + 4);
        if (!before(start, end) || !before(t->snd_una, end) || before(t->snd_max, end))
            continue;                               /* empty, old or not ours: ignored */
        if (before(start, t->snd_una))
            start = t->snd_una;
        /* Merge with what overlaps or touches, then put it in its place. */
        for (int i = 0; i < t->scored;) {
            if (!before(end, t->score_start[i]) && !before(t->score_end[i], start)) {
                if (before(t->score_start[i], start))
                    start = t->score_start[i];
                if (before(end, t->score_end[i]))
                    end = t->score_end[i];
                t->scored--;
                memmove(t->score_start + i, t->score_start + i + 1,
                        (size_t)(t->scored - i) * sizeof(uint32_t));
                memmove(t->score_end + i, t->score_end + i + 1, (size_t)(t->scored - i) * sizeof(uint32_t));
            } else {
                i++;
            }
        }
        if (t->scored == SCORE)
            continue;                               /* full: this one is left out */
        int i = 0;
        while (i < t->scored && before(t->score_start[i], start))
            i++;
        memmove(t->score_start + i + 1, t->score_start + i, (size_t)(t->scored - i) * sizeof(uint32_t));
        memmove(t->score_end + i + 1, t->score_end + i, (size_t)(t->scored - i) * sizeof(uint32_t));
        t->score_start[i] = start;
        t->score_end[i] = end;
        t->scored++;
    }
}

/* (65e) snd_una moved: drop what it passed. */
static void score_trim(struct tcp *t)
{
    int keep = 0;
    for (int i = 0; i < t->scored; i++) {
        if (!before(t->snd_una, t->score_end[i]))
            continue;
        t->score_start[keep] = before(t->score_start[i], t->snd_una) ? t->snd_una : t->score_start[i];
        t->score_end[keep++] = t->score_end[i];
    }
    t->scored = keep;
}

/* (65e) In recovery: the next hole the scoreboard shows (from resent_to on,
 * below its highest SACKed byte), sent once; false if there is none. */
static bool resend_hole(struct tcp *t, uint32_t mss)
{
    if (!t->scored)
        return false;
    uint32_t at = before(t->resent_to, t->snd_una) ? t->snd_una : t->resent_to;
    for (int i = 0; i < t->scored; i++) {
        if (!before(at, t->score_end[i]))
            continue;                               /* behind us */
        if (before(at, t->score_start[i])) {        /* [at, start) is missing */
            uint32_t n = t->score_start[i] - at;
            if (n > mss)
                n = mss;
            send_again(t, at, n);
            t->resent_to = at + n;
            COUNT(holes);
            return true;
        }
        at = t->score_end[i];                       /* inside a SACKed range: skip it */
    }
    return false;
}

/* rcv_nxt moved: take the kept ranges it reached. Under the lock. */
static void fill_gaps(struct tcp *t)
{
    for (int i = 0; i < t->gaps;) {
        if (!before(t->rcv_nxt, t->gap_start[i])) {
            if (before(t->rcv_nxt, t->gap_end[i])) {
                uint32_t more = t->gap_end[i] - t->rcv_nxt;
                t->recv_len += more;
                t->rcv_nxt += more;
            }
            drop_gap(t, i);
            i = 0;                                  /* rcv_nxt moved: look again */
        } else {
            i++;
        }
    }
}

void tcp_input(const struct inet_addr *src, const struct inet_addr *dst, const uint8_t *p,
               size_t length)
{
    if (length < 20)
        return;
    size_t header = (size_t)(p[12] >> 4) * 4;
    if (header < 20 || header > length ||
        ip_checksum_fold(ip_checksum_add(inet_pseudo(src, dst, IP_PROTO_TCP, length), p, length)) != 0)
        return;
    uint16_t src_port = get16(p), dst_port = get16(p + 2);
    uint32_t seq = get32(p + 4), ack = get32(p + 8);
    uint8_t flags = p[13];
    uint32_t window = get16(p + 14);
    const uint8_t *data = p + header;
    size_t n = length - header;

    COUNT(received);
    if (lose())
        return;
    /* (65e-35) A reply that belongs to no connection waits here until the
     * table's lock is given back. */
    struct outgoing one;
    struct outbox reply = { &one, 1, 0, 0, false };
    mutex_lock(&table_lock);
    struct tcp *t = find(src, src_port, dst_port);
    if (t && t->state == LISTEN && !(t->families & inet_family(src)))
        t = 0;                                      /* not a family the listener takes (64c) */
    if (!t) {
        reset_for(src, dst, p, n, &reply);
        mutex_unlock(&table_lock);
        send_reply(&reply);
        return;
    }

    if (t->state == LISTEN) {                       /* (under the table's lock: its backlog) */
        if (flags & RST) {
            mutex_unlock(&table_lock);
            return;
        }
        if ((flags & SYN) && !(flags & ACK)) {
            /* A SYN: answer a cookie and keep no state (49f). The MSS option
             * is encoded in the cookie, (65e-25) and the window scale and
             * SACK offers, which the answer makes only if the SYN did; the
             * connection is built only when a valid ACK returns. */
            int shift = wscale_option(p, header);
            bool sack = find_option(p, header, 4, 2) >= 0;
            uint32_t cookie = make_cookie(src, dst, src_port, dst_port, mss_option(p, header),
                                          cookie_options(shift, sack));
            struct tcp answer = { .local = *dst, .remote = *src, .local_port = dst_port,
                                  .remote_port = src_port, .rcv_nxt = seq + 1,
                                  .ws_offer = shift >= 0, .sack_offer = sack, .out = &reply };
            segment(&answer, SYN | ACK, cookie, 0, 0);
            mutex_unlock(&table_lock);
            send_reply(&reply);
            return;
        }
        /* An ACK with no matching connection: the third step of a cookie
         * handshake (or data riding on it). Build the connection only if the
         * cookie checks out; a blind or stale ACK gets a reset. */
        uint32_t peer_mss, options;
        if ((flags & ACK) && (peer_mss = check_cookie(src, dst, src_port, dst_port, ack, &options))) {
            if (t->backlog_count >= t->backlog_max) {
                reset_for(src, dst, p, n, &reply);  /* no room to accept it: refuse now */
                mutex_unlock(&table_lock);
                send_reply(&reply);
                return;
            }
            struct tcp *c = new_connection();
            if (!c) {
                mutex_unlock(&table_lock);
                return;
            }
            mutex_lock(&c->lock);                   /* (a free slot's: nobody holds it) */
            c->listener = t;
            c->local = *dst, c->remote = *src;
            c->families = t->families;
            c->own_mss = inet_mss(src);
            c->local_port = dst_port, c->remote_port = src_port;
            c->iss = ack - 1;                       /* the cookie we sent */
            c->snd_una = c->snd_nxt = c->snd_max = ack;
            c->irs = seq - 1;
            c->rcv_nxt = seq;                       /* the peer's next byte (data is retransmitted) */
            c->peer_mss = peer_mss;
            if (options & 0xF) {                    /* (65e-25) both offered: scaled */
                c->snd_shift = (uint8_t)((options & 0xF) - 1);
                c->rcv_shift = RCV_SHIFT;
                recv_start(c);
                COUNT(scaled);
            }
            if (options & 0x10) {                   /* (65e-25) both offered SACK */
                c->sack_ok = true;
                COUNT(sacked);
            }
            c->snd_wnd = window << c->snd_shift;    /* (this ACK's window is scaled) */
            c->state = ESTABLISHED;
            t->backlog[t->backlog_count++] = c;
            segment(c, ACK, c->snd_max, 0, 0);      /* ack the connection (and any unACKed data) */
            wait_queue_wake_all(&t->waiters);
            mutex_unlock(&table_lock);
            release(c);
            return;
        }
        if (!(flags & SYN))
            reset_for(src, dst, p, n, &reply);      /* a stray or bad-cookie ACK */
        mutex_unlock(&table_lock);
        send_reply(&reply);
        return;
    }
    take(t);                                        /* (65e-35) the rest is the connection's */

    if (t->state == SYN_SENT) {
        if ((flags & ACK) && ack != t->iss + 1) {
            if (!(flags & RST))
                reset_for(src, dst, p, n, &reply);
        } else if (flags & RST) {
            if (flags & ACK) {
                t->error = ERR_CONNREFUSED;
                t->state = CLOSED;
            }
        } else if (flags & SYN) {
            t->irs = seq;
            t->rcv_nxt = seq + 1;
            t->peer_mss = mss_option(p, header);
            t->snd_wnd = window;                    /* (a SYN's window is never scaled) */
            if (t->sack_offer && find_option(p, header, 4, 2) >= 0) {
                t->sack_ok = true;                  /* (65e) both offered SACK */
                COUNT(sacked);
            }
            int shift = t->ws_offer ? wscale_option(p, header) : -1;
            if (shift >= 0) {                       /* (65e) both offered: scaled */
                t->snd_shift = (uint8_t)(shift > SHIFT_MAX ? SHIFT_MAX : shift);
                t->rcv_shift = RCV_SHIFT;
                COUNT(scaled);
            }
            if (flags & ACK) {
                t->snd_una = ack;
                t->rto_at = 0;
                t->retries = 0;
                if (t->rtt_start)
                    sample(t);
                t->state = ESTABLISHED;
                segment(t, ACK, t->snd_max, 0, 0);
            } else {
                t->state = SYN_RECEIVED;            /* both opened at once */
                segment(t, SYN | ACK, t->iss, 0, 0);
            }
        }
        wait_queue_wake_all(&t->waiters);
        goto done;
    }

    /* Acceptable? Within the receive window (RFC 793's test, simplified:
     * it must start at what we expect, or be a duplicate we ack again).
     * A RST resets only on the exact next sequence number (RFC 5961): one
     * anywhere else in the window gets a challenge ACK instead of resetting
     * (a blind reset must guess rcv_nxt exactly), and one out of the window
     * is dropped. */
    if (flags & RST) {
        if (seq == t->rcv_nxt) {
            t->error = ERR_CONNRESET;
            t->state = CLOSED;
            wait_queue_wake_all(&t->waiters);
            if (t->listener && !t->user_closed)
                t->user_closed = true;              /* never accepted: free it */
        } else {
            uint32_t rwnd = t->recv_size - t->recv_len;
            if (!before(seq, t->rcv_nxt) && before(seq, t->rcv_nxt + rwnd))
                segment(t, ACK, t->snd_max, 0, 0);  /* in window, not exact: challenge ACK */
        }
        goto done;
    }
    if (flags & SYN && t->state != SYN_RECEIVED) {
        segment(t, ACK, t->snd_max, 0, 0);          /* a stray SYN: tell it where we are */
        goto done;
    }
    if (!(flags & ACK))
        goto done;

    /* The acknowledgement. */
    if (t->state == SYN_RECEIVED) {                 /* (both opened at once: never a listener's,
                                                     * whose connections start ESTABLISHED) */
        if (ack != t->iss + 1) {
            reset_for(src, dst, p, n, &reply);
            goto done;
        }
        t->snd_una = ack;
        t->rto_at = 0;
        t->retries = 0;
        t->state = ESTABLISHED;
    }
    /* RFC 5961: the ACK must be acceptable — not acknowledging data we never
     * sent, nor absurdly old. A segment whose ACK is outside that range gets
     * a challenge ACK and is otherwise ignored, so a blind segment with the
     * right four-tuple and an in-window sequence cannot move our window or
     * slip data in on a guessed sequence number alone. */
    if (before(t->snd_max, ack) || before(ack, t->snd_una - SEND_BIG)) {
        segment(t, ACK, t->snd_max, 0, 0);
        goto done;
    }
    uint32_t mss = mss_of(t);
    if (t->sack_ok)
        score_add(t, p, header);                    /* (65e) what the peer holds beyond a gap */
    if (before(t->snd_una, ack) && !before(t->snd_max, ack)) {
        uint32_t acked = ack - t->snd_una;
        bool fin_acked = t->fin_out && ack == t->fin_seq + 1;
        uint32_t data_acked = fin_acked ? acked - 1 : acked;
        if (data_acked > t->send_len)
            data_acked = t->send_len;
        t->send_len -= data_acked;
        t->snd_una = ack;
        score_trim(t);                              /* (65e) */
        if (before(t->snd_nxt, ack))
            t->snd_nxt = ack;                       /* gone back after a timeout: it had them */
        if (fin_acked)
            t->fin_sent = true;
        if (t->rtt_start && before(t->rtt_seq, ack))
            sample(t);
        t->retries = 0;
        t->dupacks = 0;
        t->rto_at = t->snd_una == t->snd_max ? 0 : now_ms() + t->rto;
        if (t->recovering) {
            if (!before(ack, t->recover)) {
                t->recovering = false;              /* all of the window is through */
                t->cwnd = t->ssthresh;
            } else {
                /* A partial acknowledgement: the next hole, at once (NewReno;
                 * 65e: the scoreboard's next, if it knows one). */
                COUNT(fast);
                if (!resend_hole(t, mss))
                    send_again(t, t->snd_una, mss);
                t->cwnd = t->cwnd > acked ? t->cwnd - acked + mss : mss;
            }
        } else if (t->cwnd < t->ssthresh) {
            t->cwnd += mss;                         /* slow start: doubles per round trip */
        } else {
            t->cwnd += mss * mss / t->cwnd + 1;     /* then one MSS per round trip */
        }
        if (t->cwnd > t->send_size)
            t->cwnd = t->send_size;                 /* (65e) no more in flight than it holds */
        if (fin_acked) {
            if (t->state == FIN_WAIT_1) {
                t->state = FIN_WAIT_2;
                if (t->user_closed)                 /* (53a: not after a shutdown: it reads on) */
                    t->time_wait_until = now_ms() + FIN_WAIT2_MS;   /* a peer FIN resets it (49e) */
            } else if (t->state == CLOSING) {
                t->state = TIME_WAIT;
                t->time_wait_until = now_ms() + TIME_WAIT_MS;
            } else if (t->state == LAST_ACK) {
                t->state = CLOSED;
            }
        }
    } else if (ack == t->snd_una && n == 0 && !(flags & FIN) &&
               (window << t->snd_shift) == t->snd_wnd && t->snd_max != t->snd_una) {
        /* (65e: the window compared scaled; unscaled since 65e-15, no
         * duplicate was ever seen on a scaled connection) */
        /* A duplicate: the peer got something after a gap. */
        if (++t->dupacks == 3 && !t->recovering) {
            uint32_t flight = t->snd_max - t->snd_una;
            t->ssthresh = flight / 2 > 2 * mss ? flight / 2 : 2 * mss;
            t->recover = t->snd_max;
            t->recovering = true;
            t->rtt_start = 0;                       /* Karn */
            COUNT(fast);
            send_again(t, t->snd_una, mss);
            t->resent_to = t->snd_una + mss;        /* (65e) the first hole is out */
            t->cwnd = t->ssthresh + 3 * mss;
            t->rto_at = now_ms() + t->rto;
        } else if (t->dupacks > 3 && t->recovering) {
            t->cwnd += mss;                         /* one more left the network */
            resend_hole(t, mss);                    /* (65e) and the next hole goes */
        }
    }
    if (window == 0 && ack == t->snd_una)
        t->retries = 0;                             /* full, but there */
    t->snd_wnd = window << t->snd_shift;            /* (65e: 0 unless scaled) */

    /* The data. What came before rcv_nxt already is cut off (a segment
     * sent again, perhaps cut differently). */
    bool ack_now = false, in_order = false, at_once = false;   /* (65e) */
    if (n > 0 && before(seq, t->rcv_nxt)) {
        uint32_t old = t->rcv_nxt - seq;
        if (old >= n) {
            n = 0;                                  /* all of it seen before */
            ack_now = true;
            flags &= (uint8_t)~FIN;
        } else {
            data += old, n -= old, seq = t->rcv_nxt;
        }
    }
    if (n > 0 && t->user_closed && (t->state == FIN_WAIT_1 || t->state == FIN_WAIT_2 ||
                                    t->state == CLOSING)) {
        /* (36e-8) New data after the program closed: nobody will read it,
         * a RST (RFC 1122 4.2.2.13; Linux resets in FIN_WAIT with the
         * reading side shut). */
        segment(t, RST | ACK, t->snd_max, 0, 0);
        t->state = CLOSED;
        COUNT(resets_unread);
        goto done;
    }
    if (n > 0 && !t->peer_fin) {
        uint32_t offset = seq - t->rcv_nxt, room = t->recv_size - t->recv_len;
        if (offset < room) {
            size_t take = n < room - offset ? n : room - offset;
            size_t at = (t->recv_head + t->recv_len + offset) % t->recv_size;
            size_t first = t->recv_size - at < take ? t->recv_size - at : take;    /* (65e) */
            memcpy(t->recv_buf + at, data, first);
            memcpy(t->recv_buf, data + first, take - first);
            if (take < n)
                flags &= (uint8_t)~FIN;             /* its FIN comes again with the rest */
            if (offset == 0) {
                bool had_gaps = t->gaps > 0;
                t->recv_len += (uint32_t)take;
                t->rcv_nxt += (uint32_t)take;
                fill_gaps(t);
                in_order = true;
                grow_check(t);                      /* (65e) */
                /* (65e-29) A full segment's ACK may wait for the next; not a
                 * short one's (the end of a write: an answer is waited for),
                 * one that fills a gap (RFC 5681: at once) or the first ones
                 * (the sender's slow start, as Linux's quick ACKs). */
                if (t->own_mss && take >= t->own_mss && !had_gaps &&
                    t->rcv_nxt - t->irs > QUICK_SEGMENTS * t->own_mss)
                    t->full_owed++;
                else
                    t->ack_soon = true;
            } else {
                at_once = true;                     /* a duplicate ACK: the sender counts them */
                keep_gap(t, seq, seq + (uint32_t)take);
                flags &= (uint8_t)~FIN;             /* taken in order only */
            }
        }
        ack_now = true;                             /* also for one after a gap: a duplicate */
    }
    if ((flags & FIN) && seq + (uint32_t)n == t->rcv_nxt && !t->peer_fin) {
        t->rcv_nxt++;
        t->peer_fin = true;
        ack_now = true;
        at_once = true;
        if (t->state == ESTABLISHED)
            t->state = CLOSE_WAIT;
        else if (t->state == FIN_WAIT_1)
            t->state = CLOSING;
        else if (t->state == FIN_WAIT_2) {
            t->state = TIME_WAIT;
            t->time_wait_until = now_ms() + TIME_WAIT_MS;
        }
    } else if ((flags & FIN) && t->peer_fin) {
        ack_now = true;                             /* sent again: our ACK was lost */
    }
    /* (65e) In a card's burst, data that came in order is acknowledged at
     * its end (tcp_batch_end); anything else at once, as before. */
    if (ack_now && in_order && !at_once && thread_current()->net_batch) {
        t->ack_owed = true;
        t->owed_by = thread_current();
    } else if (ack_now) {
        segment(t, ACK, t->snd_max, 0, 0);
    }
    output(t);
    wait_queue_wake_all(&t->waiters);
done:;
    bool gone = dead(t);
    release(t);
    send_reply(&reply);
    if (gone)
        free_if_dead(t);
}
