/*
 * sys/resource.h (43a-3): limits and use. getrlimit gives RitterOS's
 * fixed limits (RLIMIT_NOFILE 256 descriptors, RLIMIT_STACK 8 MiB,
 * RLIMIT_NPROC 256 threads; core files and locked memory 0; the rest
 * RLIM_INFINITY); setrlimit takes lower ones (believed, the system's own
 * still hold) and refuses higher ones but for root. getrusage: the
 * program's CPU time as ru_utime (RUSAGE_SELF; the children's is not
 * kept). getpriority/setpriority: there are no priorities (0).
 */
#ifndef RITTEROS_SYS_RESOURCE_H
#define RITTEROS_SYS_RESOURCE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/time.h>
#include <sys/types.h>

typedef unsigned long rlim_t;
#define RLIM_INFINITY           (~0UL)
#define RLIM_SAVED_MAX          RLIM_INFINITY
#define RLIM_SAVED_CUR          RLIM_INFINITY
struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};
#define RLIMIT_CPU              0
#define RLIMIT_FSIZE            1
#define RLIMIT_DATA             2
#define RLIMIT_STACK            3
#define RLIMIT_CORE             4
#define RLIMIT_RSS              5
#define RLIMIT_NPROC            6
#define RLIMIT_NOFILE           7
#define RLIMIT_MEMLOCK          8
#define RLIMIT_AS               9
#define RLIMIT_LOCKS            10
#define RLIMIT_SIGPENDING       11
#define RLIMIT_MSGQUEUE         12
#define RLIMIT_NICE             13
#define RLIMIT_RTPRIO           14
#define RLIMIT_RTTIME           15
#define RLIMIT_NLIMITS          16

struct rusage {
    struct timeval ru_utime, ru_stime;
    long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap, ru_inblock,
         ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw;
};
#define RUSAGE_SELF             0
#define RUSAGE_CHILDREN         (-1)
#define RUSAGE_THREAD           1

#define PRIO_PROCESS            0
#define PRIO_PGRP               1
#define PRIO_USER               2

int getrlimit(int resource, struct rlimit *limit);
int setrlimit(int resource, const struct rlimit *limit);
int getrusage(int who, struct rusage *usage);
int getpriority(int which, id_t who);
int setpriority(int which, id_t who, int priority);

#ifdef __cplusplus
}
#endif

#endif
