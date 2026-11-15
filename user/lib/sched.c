/*
 * (43a-4) sched.h's calls: one policy (SCHED_OTHER) without priorities,
 * every thread on every core (sched_getaffinity: the cores started).
 */
#include <errno.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

int sched_get_priority_max(int policy)
{
    if (policy < 0 || policy > SCHED_IDLE) {
        errno = EINVAL;
        return -1;
    }
    return policy == SCHED_FIFO || policy == SCHED_RR ? 99 : 0;
}

int sched_get_priority_min(int policy)
{
    if (policy < 0 || policy > SCHED_IDLE) {
        errno = EINVAL;
        return -1;
    }
    return policy == SCHED_FIFO || policy == SCHED_RR ? 1 : 0;
}

int sched_getparam(pid_t pid, struct sched_param *param)
{
    (void)pid;
    param->sched_priority = 0;
    return 0;
}

int sched_setparam(pid_t pid, const struct sched_param *param)
{
    (void)pid;
    if (param->sched_priority != 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int sched_getscheduler(pid_t pid)
{
    (void)pid;
    return SCHED_OTHER;
}

int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param)
{
    (void)pid;
    if (policy == SCHED_FIFO || policy == SCHED_RR) {
        errno = EPERM;
        return -1;
    }
    if ((policy != SCHED_OTHER && policy != SCHED_BATCH && policy != SCHED_IDLE) || param->sched_priority) {
        errno = EINVAL;
        return -1;
    }
    return SCHED_OTHER;
}

int sched_rr_get_interval(pid_t pid, struct timespec *interval)
{
    (void)pid;
    interval->tv_sec = 0;
    interval->tv_nsec = 10000000;               /* (a tick) */
    return 0;
}

int __ritteros_cpu_count(size_t size, const cpu_set_t *set)
{
    int n = 0;
    for (size_t i = 0; i < size / sizeof(unsigned long); i++)
        n += __builtin_popcountl(set->bits[i]);
    return n;
}

int sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set)
{
    (void)pid;
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    memset(set, 0, size);
    for (long i = 0; i < cores && (size_t)i < size * 8; i++)
        set->bits[i / 64] |= 1UL << (i % 64);
    return 0;
}

int sched_setaffinity(pid_t pid, size_t size, const cpu_set_t *set)
{
    (void)pid;
    if (__ritteros_cpu_count(size, set) == 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int sched_getcpu(void)
{
    errno = ENOSYS;
    return -1;
}
