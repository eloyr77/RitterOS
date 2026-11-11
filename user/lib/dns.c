/*
 * DNS messages (RFC 1035), for the elf dnsd and the library's resolver
 * (net.c). A query is a 12-byte header (an ID, "recursion desired", one
 * question) and the question: the name as length-prefixed labels
 * ("\7example\3com\0"), type A (1), class IN (1). An answer repeats the
 * question and adds records; names in it may point back into the message
 * (compression: two bytes 0xC0 | offset). The first A record is the
 * address; (64d) the first AAAA record (type 28, 16 bytes) an IPv6 one.
 */
#include <errno.h>
#include <string.h>
#include "ritteros.h"

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v;
}

size_t dns_query(const char *name, uint16_t id, uint8_t *out, size_t size)
{
    return dns_query_type(name, id, 1, out, size);
}

size_t dns_query_type(const char *name, uint16_t id, uint16_t type, uint8_t *out, size_t size)
{
    size_t n = strlen(name);
    if (n == 0 || n > DNS_NAME_MAX || size < 12 + n + 2 + 4)
        return 0;
    memset(out, 0, 12);
    put16(out, id);
    put16(out + 2, 0x0100);                         /* recursion desired */
    put16(out + 4, 1);                              /* one question */
    size_t at = 12;
    for (const char *label = name; *label;) {
        const char *end = label;
        while (*end && *end != '.')
            end++;
        size_t length = (size_t)(end - label);
        if (length == 0 || length > 63)
            return 0;
        out[at++] = (uint8_t)length;
        memcpy(out + at, label, length);
        at += length;
        label = *end ? end + 1 : end;
    }
    out[at++] = 0;
    put16(out + at, type), put16(out + at + 2, 1);  /* the type (A: 1), class IN */
    return at + 4;
}

/* Past a (possibly compressed) name at *at; false if it runs off. */
static bool skip_name(const uint8_t *m, size_t length, size_t *at)
{
    while (*at < length) {
        uint8_t n = m[*at];
        if (n == 0) {
            (*at)++;
            return true;
        }
        if ((n & 0xC0) == 0xC0) {
            *at += 2;                               /* a pointer ends the name */
            return *at <= length;
        }
        *at += 1 + n;
    }
    return false;
}

/* The first record of `type` with `size` bytes of data in an answer: 0
 * and its data into out, or the errno dns_answer gives. */
static int first_record(const uint8_t *m, size_t length, uint16_t want, uint16_t size,
                        uint8_t *out, uint32_t *ttl)
{
    if (length < 12 || !(m[2] & 0x80))
        return EINVAL;                              /* not an answer */
    int rcode = m[3] & 0x0F;
    if (rcode == 3)
        return ENONAME;                             /* no such name */
    if (rcode == 5)
        return ENETUNREACH;                         /* refused: dnsd knows no server */
    if (rcode != 0)
        return ETIMEDOUT;                           /* server failure: none answered dnsd */
    size_t at = 12;
    for (int q = get16(m + 4); q > 0; q--)
        if (!skip_name(m, length, &at) || (at += 4) > length)
            return EINVAL;
    for (int a = get16(m + 6); a > 0; a--) {
        if (!skip_name(m, length, &at) || at + 10 > length)
            return EINVAL;
        uint16_t type = get16(m + at), rdlength = get16(m + at + 8);
        uint32_t seconds = (uint32_t)get16(m + at + 4) << 16 | get16(m + at + 6);
        at += 10;
        if (at + rdlength > length)
            return EINVAL;
        if (type == want && rdlength == size) {
            memcpy(out, m + at, size);
            *ttl = seconds;
            return 0;
        }
        at += rdlength;                             /* a CNAME: the record follows */
    }
    return ENONAME;
}

int dns_answer(const uint8_t *m, size_t length, uint32_t *address, uint32_t *ttl)
{
    uint8_t a[4];
    int r = first_record(m, length, 1, 4, a, ttl);
    if (!r)
        *address = (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
    return r;
}

int dns_answer_aaaa(const uint8_t *m, size_t length, uint8_t address[16], uint32_t *ttl)
{
    return first_record(m, length, 28, 16, address, ttl);
}

size_t dns_failure(const uint8_t *query, size_t length, int rcode, uint8_t *out, size_t size)
{
    if (length < 12 || length > size)
        return 0;
    memcpy(out, query, length);
    put16(out + 2, (uint16_t)(0x8180 | rcode));     /* an answer, no records */
    put16(out + 6, 0), put16(out + 8, 0), put16(out + 10, 0);
    return length;
}

bool dns_question(const uint8_t *m, size_t length, char *name, size_t size)
{
    uint16_t type;
    return dns_question_type(m, length, name, size, &type) && type == 1;
}

bool dns_question_type(const uint8_t *m, size_t length, char *name, size_t size, uint16_t *type)
{
    if (length < 12 || (m[2] & 0x80) || get16(m + 4) != 1)
        return false;
    size_t at = 12, used = 0;
    while (at < length && m[at]) {
        uint8_t n = m[at++];
        if (n > 63 || at + n > length || used + n + 2 > size)
            return false;
        if (used)
            name[used++] = '.';
        memcpy(name + used, m + at, n);
        used += n;
        at += n;
    }
    if (at + 5 > length || get16(m + at + 3) != 1)
        return false;                               /* only class IN */
    *type = get16(m + at + 1);
    name[used] = 0;
    return used > 0;
}

/* The question as it came (header and question), then one record of
 * `type` whose name points back at the question's (0xC00C). */
static size_t reply_record(const uint8_t *query, size_t length, uint16_t type, const uint8_t *data,
                           uint16_t data_size, uint32_t ttl, uint8_t *out, size_t size)
{
    size_t at = 12;
    if (!skip_name(query, length, &at) || at + 4 > length || at + 4 + 12 + (size_t)data_size > size)
        return 0;
    at += 4;
    memcpy(out, query, at);
    put16(out + 2, 0x8180);                         /* an answer, recursion available */
    put16(out + 6, 1);                              /* one record */
    put16(out + 8, 0), put16(out + 10, 0);
    uint8_t *r = out + at;
    put16(r, 0xC00C);
    put16(r + 2, type), put16(r + 4, 1);
    put16(r + 6, (uint16_t)(ttl >> 16)), put16(r + 8, (uint16_t)ttl);
    put16(r + 10, data_size);
    memcpy(r + 12, data, data_size);
    return at + 12 + data_size;
}

size_t dns_reply(const uint8_t *query, size_t length, uint32_t address, uint32_t ttl,
                 uint8_t *out, size_t size)
{
    uint8_t a[4] = { (uint8_t)(address >> 24), (uint8_t)(address >> 16), (uint8_t)(address >> 8),
                     (uint8_t)address };
    return reply_record(query, length, 1, a, 4, ttl, out, size);
}

size_t dns_reply_aaaa(const uint8_t *query, size_t length, const uint8_t address[16], uint32_t ttl,
                      uint8_t *out, size_t size)
{
    return reply_record(query, length, 28, address, 16, ttl, out, size);
}
