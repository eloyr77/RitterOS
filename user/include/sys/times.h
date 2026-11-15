/*
 * sys/times.h (43a-3): times: the program's CPU time in clock ticks
 * (sysconf(_SC_CLK_TCK), 100 a second) as tms_utime (the children's is
 * not kept); the result: ticks since the start.
 */
#ifndef RITTEROS_SYS_TIMES_H
#define RITTEROS_SYS_TIMES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

struct tms {
    clock_t tms_utime, tms_stime, tms_cutime, tms_cstime;
};
clock_t times(struct tms *t);

#ifdef __cplusplus
}
#endif

#endif
