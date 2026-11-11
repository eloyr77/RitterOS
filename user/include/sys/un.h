/* Local (Unix-domain) socket addresses (53a): a name of up to 107 bytes
 * in RitterOS's registry of listeners (no file is made). */
#ifndef RITTEROS_SYS_UN_H
#define RITTEROS_SYS_UN_H

#include <sys/socket.h>

struct sockaddr_un {
    sa_family_t sun_family;     /* AF_UNIX */
    char sun_path[108];
};

#endif
