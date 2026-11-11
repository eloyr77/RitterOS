/* TCP's socket options (53a): TCP_NODELAY at IPPROTO_TCP is kept and
 * changes nothing (RitterOS sends a segment at once anyway). */
#ifndef RITTEROS_NETINET_TCP_H
#define RITTEROS_NETINET_TCP_H

#include <netinet/in.h>

#define TCP_NODELAY     1

#endif
