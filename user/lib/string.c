#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* memcpy, memmove, memset, memcmp, memchr, memrchr, strlen, strchr,
 * strrchr, strcmp and strncmp are assembly: user/arch/<arch>/string.S
 * (32e: AVX2 and SSE2 on x86-64; 32g: NEON on AArch64). */
#if defined(__x86_64__)
/* (32e) Which versions user/arch/x86_64/string.S's functions take: 2 AVX2,
 * 1 SSE2, 0 not asked yet (their first call asks here). AVX2 needs the
 * processor's AVX and AVX2 and the system saving the YMM registers (XCR0's
 * SSE and AVX bits). Nothing here may call a string function. */
__attribute__((visibility("hidden"))) unsigned char __ritteros_simd;

__attribute__((visibility("hidden"))) void __ritteros_simd_detect(void)
{
    unsigned a, b, c, d, lo, hi, top;
    __asm__ volatile ("cpuid" : "=a"(top), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    int avx2 = 0;
    if (top >= 7 && (c >> 27 & 1) && (c >> 28 & 1)) {          /* OSXSAVE, AVX */
        __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
        avx2 = (lo & 6) == 6 && (b >> 5 & 1);
    }
    __atomic_store_n(&__ritteros_simd, avx2 ? 2 : 1, __ATOMIC_RELAXED);
}
#endif

char *strcpy(char *dst, const char *src)
{
    memcpy(dst, src, strlen(src) + 1);
    return dst;
}

char *stpcpy(char *dst, const char *src)
{
    size_t n = strlen(src);
    memcpy(dst, src, n + 1);
    return dst + n;
}

const char *strerror(int error)
{
    switch (error) {
    case 0:            return "Success";
    case EPERM:        return "Operation not permitted";
    case ENOENT:       return "No such file or directory";
    case EACCES:       return "Permission denied";
    case ESRCH:        return "No such process";
    case EINTR:        return "Interrupted system call";
    case EIO:          return "Input/output error";
    case E2BIG:        return "Argument list too long";
    case ENOEXEC:      return "Exec format error";
    case EBADF:        return "Bad file descriptor";
    case ECHILD:       return "No child processes";
    case ENOMEM:       return "Cannot allocate memory";
    case EFAULT:       return "Bad address";
    case EEXIST:       return "File exists";
    case ENODEV:       return "No such device";
    case ENXIO:        return "Device not configured";
    case ENOTTY:       return "Inappropriate ioctl for device";
    case ENOTSOCK:     return "Socket operation on non-socket";
    case ELOOP:        return "Too many levels of symbolic links";
    case EDESTADDRREQ: return "Destination address required";
    case EMSGSIZE:     return "Message too long";
    case ENOPROTOOPT:  return "Protocol not available";
    case EPROTONOSUPPORT: return "Protocol not supported";
    case ESOCKTNOSUPPORT: return "Socket type not supported";
    case EAFNOSUPPORT: return "Address family not supported by protocol family";
    case EADDRNOTAVAIL: return "Can't assign requested address";
    case ECONNABORTED: return "Software caused connection abort";
    case EISCONN:      return "Socket is already connected";
    case EALREADY:     return "Operation already in progress";
    case EDEADLK:      return "Resource deadlock avoided";
    case EINPROGRESS:  return "Operation now in progress";
    case ENOTDIR:      return "Not a directory";
    case EISDIR:       return "Is a directory";
    case EINVAL:       return "Invalid argument";
    case EMFILE:       return "Too many open files";
    case EROFS:        return "Read-only file system";
    case ENAMETOOLONG: return "File name too long";
    case ENOSYS:       return "Function not implemented";
    case ENOTEMPTY:    return "Directory not empty";
    case ENOSPC:       return "No space left on device";
    case ENODATA:      return "No such attribute";
    case EBUSY:        return "Device or resource busy";
    case EXDEV:        return "Invalid cross-device link";
    case ENOTSUP:      return "Operation not supported";
    case ENETUNREACH:  return "Network is unreachable";
    case ETIMEDOUT:    return "Timed out";
    case ERANGE:       return "Result out of range";
    case EILSEQ:       return "Invalid or incomplete multibyte or wide character";
    case EHOSTUNREACH: return "No route to host";
    case EPIPE:        return "Broken pipe";
    case EADDRINUSE:   return "Address already in use";
    case ECONNRESET:   return "Connection reset by peer";
    case ENOTCONN:     return "Not connected";
    case ECONNREFUSED: return "Connection refused";
    case ENONAME:      return "Name not found";
    default:           return "Unknown error";
    }
}

long atol(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    bool negative = *s == '-';
    if (*s == '-' || *s == '+')
        s++;
    long value = 0;
    while (*s >= '0' && *s <= '9')
        value = value * 10 + (*s++ - '0');
    return negative ? -value : value;
}

int atoi(const char *s)
{
    return (int)atol(s);
}

char *strstr(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    for (; *haystack; haystack++)
        if (strncmp(haystack, needle, n) == 0)
            return (char *)haystack;
    return n == 0 ? (char *)haystack : 0;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}
