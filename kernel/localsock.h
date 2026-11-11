/*
 * Local (Unix-domain) stream sockets: a connection between two programs on
 * the same machine, named by a path-like string (a key in a kernel
 * registry, not a file on the disk — normal boots never write). They are
 * the local transport the window system is built on (windowd and its
 * clients), useful for any two programs that talk on one machine.
 *
 * A listener binds a name and accepts connections; a client connects to the
 * name. Each connection carries two byte streams (one per direction), read
 * and written like a TCP connection. Everything runs in process context
 * (no device, no interrupt); the streams' locks and wait queues carry the
 * data between the two threads.
 */
#ifndef RITTEROS_LOCALSOCK_H
#define RITTEROS_LOCALSOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "syscalls.h"           /* LOCAL_PATH_MAX */

struct local;

/* Give a new endpoint the name it will listen on (before local_listen), for
 * user uid (local_peer_uid tells it to its clients). *l must be 0. 0 or ERR_*. */
int local_bind(struct local **l, const char *name, int uid);

/* Register the bound name and start accepting (backlog clamped to 1..128).
 * ERR_ADDRINUSE if the name is taken. 0 or ERR_*. */
int local_listen(struct local **l, int backlog);

/* Connect a new endpoint to a listener's name. *l must be 0. The connection
 * is live at once (the server picks it up with local_accept).
 * ERR_CONNREFUSED if nobody listens on that name. 0 or ERR_*. */
int local_connect(struct local **l, const char *name, int uid);

/* Take the next pending connection as a new endpoint (*out). Waits until one
 * arrives or the thread is stopped. 0 or ERR_*. */
int local_accept(struct local *l, struct local **out);

/* Send on a connected endpoint; bytes written or -ERR_* (-ERR_PIPE once the
 * peer has gone). Blocks while the stream is full. */
int64_t local_send(struct local *l, const void *data, size_t length, uint32_t timeout_ms);
/* (53a) Its own name (a listener's, or the server end's) or its peer's
 * (the client end's), "" for none; shutdown's sides. */
int local_sock_name(struct local *l, bool peer, char *out, size_t size);
void local_shutdown(struct local *l, bool read, bool write);

/* Receive on a connected endpoint; bytes, 0 once the peer closed and nothing
 * is left, or -ERR_* (timeout_ms 0: wait as long as it takes). */
int64_t local_recv(struct local *l, void *data, size_t size, uint32_t timeout_ms);

/* Pass a shared buffer to the peer / take one the peer passed (fd-passing,
 * like Unix SCM_RIGHTS). The queued reference is the caller's to manage:
 * send takes a held ref (and returns ERR_AGAIN, not consuming it, when the
 * queue is full); recv hands its ref to the caller (0 when none waits). */
struct shbuf;
int local_send_buffer(struct local *l, struct shbuf *b);
struct shbuf *local_recv_buffer(struct local *l);

/* The listener's name, or "" for a connected endpoint. */
const char *local_name(struct local *l);

/* The user at the other end of a connected endpoint (the client's, or the
 * listener's who bound the name), or -1 when not connected: how a server and
 * its clients tell who they talk to (the window system, 15k). */
int local_peer_uid(struct local *l);

/* Close an endpoint: a listener leaves the registry and refuses its pending
 * connections; a connected endpoint tells its peer it is gone. */
void local_close(struct local *l);

/* (52d) For poll: POLLIN (bytes, the peer's end, or a connection to
 * accept), POLLOUT (room), POLLHUP (the writer closed), POLLERR (the
 * reader left). */
int local_poll(struct local *l);

#endif
