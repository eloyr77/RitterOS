/* (54a) Giving the processor away; (43a-4) POSIX's scheduling calls and
 * GNU's core sets. RitterOS's scheduler has one policy without priorities
 * (SCHED_OTHER, priority 0); asking for another is EINVAL (SCHED_FIFO,
 * SCHED_RR: EPERM, as for a user without rights); a thread may run on
 * every core: sched_getaffinity gives them all, sched_setaffinity is
 * believed but changes nothing. sched_getcpu is not known (-1, ENOSYS). */
#ifndef RITTEROS_SCHED_H
#define RITTEROS_SCHED_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

/* Let other threads and programs run first; 0. */
int sched_yield(void);

#define SCHED_OTHER     0
#define SCHED_FIFO      1
#define SCHED_RR        2
#define SCHED_BATCH     3
#define SCHED_IDLE      5
struct sched_param {
    int sched_priority;
};
int sched_get_priority_max(int policy);
int sched_get_priority_min(int policy);
int sched_getparam(pid_t pid, struct sched_param *param);
int sched_setparam(pid_t pid, const struct sched_param *param);
int sched_getscheduler(pid_t pid);
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);
int sched_rr_get_interval(pid_t pid, struct timespec *interval);

/* A set of cores, GNU's: up to 1024. */
#define CPU_SETSIZE     1024
typedef struct {
    unsigned long bits[CPU_SETSIZE / 64];
} cpu_set_t;
#define CPU_ZERO(set)           __builtin_memset((set), 0, sizeof(cpu_set_t))
#define CPU_SET(n, set)         ((void)((n) < CPU_SETSIZE && ((set)->bits[(n) / 64] |= 1UL << ((n) % 64))))
#define CPU_CLR(n, set)         ((void)((n) < CPU_SETSIZE && ((set)->bits[(n) / 64] &= ~(1UL << ((n) % 64)))))
#define CPU_ISSET(n, set)       ((n) < CPU_SETSIZE && ((set)->bits[(n) / 64] >> ((n) % 64) & 1))
#define CPU_COUNT(set)          __ritteros_cpu_count(sizeof(cpu_set_t), (set))
#define CPU_EQUAL(a, b)         (__builtin_memcmp((a), (b), sizeof(cpu_set_t)) == 0)
int __ritteros_cpu_count(size_t size, const cpu_set_t *set);
int sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set);
int sched_setaffinity(pid_t pid, size_t size, const cpu_set_t *set);
int sched_getcpu(void);

#ifdef __cplusplus
}
#endif

#endif
