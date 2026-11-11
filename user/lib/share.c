/*
 * Shared buffers for programs (SYS_SHARE): make a buffer, map it into this
 * program's memory, and pass it to another program over a local socket. The
 * window system uses them for pixels; any two local programs can share bytes
 * without copying.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "ritteros.h"

static int64_t check(int64_t r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

int ritteros_share_create(size_t size)
{
    return (int)check(syscall6(SYS_SHARE, SHARE_CREATE, (int64_t)size, 0, 0, 0, 0));
}

void *ritteros_share_map(int fd)
{
    int64_t r = check(syscall6(SYS_SHARE, SHARE_MAP, fd, 0, 0, 0, 0));
    return r < 0 ? 0 : (void *)(uintptr_t)r;
}

long ritteros_share_size(int fd)
{
    return (long)check(syscall6(SYS_SHARE, SHARE_SIZE, fd, 0, 0, 0, 0));
}

int ritteros_share_send(int socket_fd, int buffer_fd)
{
    return (int)check(syscall6(SYS_SHARE, SHARE_SEND, socket_fd, buffer_fd, 0, 0, 0));
}

int ritteros_share_recv(int socket_fd)
{
    return (int)check(syscall6(SYS_SHARE, SHARE_RECV, socket_fd, 0, 0, 0, 0));
}

/* The screen (SYS_DISPLAY), for the window server. */
int ritteros_display_acquire(void)
{
    return (int)check(syscall6(SYS_DISPLAY, DISPLAY_ACQUIRE, 0, 0, 0, 0, 0));
}

int ritteros_display_release(void)
{
    return (int)check(syscall6(SYS_DISPLAY, DISPLAY_RELEASE, 0, 0, 0, 0, 0));
}

int ritteros_display_info(struct sys_display *info)
{
    return (int)check(syscall6(SYS_DISPLAY, DISPLAY_INFO, (int64_t)info, 0, 0, 0, 0));
}

int ritteros_display_modes(struct sys_display_modes *modes)
{
    return (int)check(syscall6(SYS_DISPLAY, DISPLAY_MODES, (int64_t)modes, 0, 0, 0, 0));
}

int ritteros_display_set_mode(const char *output, uint32_t width, uint32_t height,
                              uint32_t refresh_mhz, char *said, size_t said_size)
{
    struct sys_display_set_mode m = { { 0 }, width, height, refresh_mhz, { 0 } };
    strncpy(m.output, output, sizeof(m.output) - 1);
    int r = (int)check(syscall6(SYS_DISPLAY, DISPLAY_SET_MODE, (int64_t)&m, 0, 0, 0, 0));
    if (said && said_size) {
        m.said[sizeof(m.said) - 1] = 0;
        strncpy(said, m.said, said_size - 1);
        said[said_size - 1] = 0;
    }
    return r;
}

int ritteros_display_vblank(uint64_t *frame, uint32_t timeout_ms)
{
    uint64_t f = 0;
    int r = (int)check(syscall6(SYS_DISPLAY, DISPLAY_VBLANK, (int64_t)&f, timeout_ms, 0, 0, 0));
    if (r > 0 && frame)
        *frame = f;
    return r;
}

int ritteros_display_buffers(struct sys_display_buffers *buffers)
{
    return (int)check(syscall6(SYS_DISPLAY, DISPLAY_BUFFERS, (int64_t)buffers, 0, 0, 0, 0));
}

int ritteros_display_flip(int buffer, uint64_t *frame)
{
    uint64_t f = 0;
    int r = (int)check(syscall6(SYS_DISPLAY, DISPLAY_FLIP, buffer, (int64_t)&f, 0, 0, 0));
    if (r > 0 && frame)
        *frame = f;
    return r;
}

int ritteros_display_flush(const void *pixels, uint32_t stride, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h)
{
    struct sys_display_flush f = { (uint64_t)(uintptr_t)pixels, stride, x, y, w, h };
    return (int)check(syscall6(SYS_DISPLAY, DISPLAY_FLUSH, (int64_t)&f, 0, 0, 0, 0));
}

int ritteros_display_event(struct sys_input_event *event, uint32_t timeout_ms)
{
    return (int)check(syscall6(SYS_DISPLAY, DISPLAY_EVENT, (int64_t)event, timeout_ms, 0, 0, 0));
}
