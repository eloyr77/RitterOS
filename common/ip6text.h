/*
 * IPv6 addresses as text (64a), shared by the kernel and the programs'
 * library (inet_pton, inet_ntop): no kernel or library calls inside.
 */
#ifndef RITTEROS_IP6TEXT_H
#define RITTEROS_IP6TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IP6_TEXT_MAX 46         /* the longest form and its NUL (INET6_ADDRSTRLEN) */

/* The address's text into out, as BSD's inet_ntop writes it (RFC 5952:
 * lower case, no leading zeros, the longest run of two or more zero
 * groups as "::", the first of equal ones; ::ffff:a.b.c.d and ::a.b.c.d
 * with the IPv4 part dotted): its length, or 0 when size is too small. */
size_t ip6_format(const uint8_t address[16], char *out, size_t size);

/* The n bytes of text as an address (RFC 4291's forms: eight groups of
 * one to four hex digits, "::" once for a run of zeros, the last 32 bits
 * as a.b.c.d); false if it is not one (nothing after it: no "%card"). */
bool ip6_parse(const char *text, size_t n, uint8_t out[16]);

#endif
