/*
 * Test: the cryptography for SSH (crypto.h) on the RFCs' inputs and on
 * inputs derived from SHA-512 of labels; scripts/crypto_ref.py expected
 * prints the same lines from a Python reference (checked against the
 * RFCs' literal outputs). Then the time per operation.
 */
#include <stdio.h>
#include <string.h>
#include "crypto.h"
#include "ritteros.h"

static char big[1000000];
static uint8_t data[1024], out[1024];

static void show(const char *what, const uint8_t *p, size_t n)
{
    printf("%s: ", what);
    for (size_t i = 0; i < n; i++)
        printf("%02x", p[i]);
    printf("\n");
}

static size_t hex(uint8_t *out, const char *text)
{
    size_t n = 0;
    for (; text[0] && text[1]; text += 2) {
        int v = 0;
        for (int i = 0; i < 2; i++) {
            char c = text[i];
            v = v * 16 + (c >= 'a' ? c - 'a' + 10 : c - '0');
        }
        out[n++] = (uint8_t)v;
    }
    return n;
}

static const char *const ed25519_tests[3][3] = {        /* RFC 8032 7.1: seed, public, message */
    { "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
      "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "" },
    { "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
      "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72" },
    { "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
      "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82" },
};

static long milliseconds(void)
{
    return (long)(ritteros_time() / 1000000);
}

int main(void)
{
    uint8_t h[64], key[32], block[16], s[32], p[32], sig[64];
    sha512("", 0, h);
    show("sha512('')", h, 64);
    sha512("abc", 3, h);
    show("sha512('abc')", h, 64);
    memset(big, 'a', sizeof(big));
    sha512(big, sizeof(big), h);
    show("sha512(a million 'a')", h, 64);

    for (int i = 0; i < 32; i++)
        key[i] = (uint8_t)i;
    memset(block, 0, 16);
    hex(block, "01000000000000090000004a00000000");
    memset(data, 0, 64);
    chacha20(key, block, data, out, 64);
    show("chacha20 RFC 8439 2.3.2", out, 64);
    const char *sun = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip "
                      "for the future, sunscreen would be it.";
    hex(block, "01000000000000000000004a00000000");
    chacha20(key, block, sun, out, strlen(sun));
    show("chacha20 RFC 8439 2.4.2", out, strlen(sun));
    hex(block, "ffffffff000000000000000000000000");
    memset(data, 0, 128);
    chacha20(key, block, data, out, 128);
    show("chacha20 64-bit counter", out, 128);
    hex(key, "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
    const char *forum = "Cryptographic Forum Research Group";
    poly1305(key, forum, strlen(forum), h);
    show("poly1305 RFC 8439 2.5.2", h, 16);

    hex(s, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
    hex(p, "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
    x25519(out, s, p);
    show("x25519 RFC 7748 5.2", out, 32);
    memset(s, 0, 32);
    s[0] = 9;
    x25519(out, s, s);
    show("x25519 RFC 7748 5.2, 1 iteration", out, 32);
    uint8_t alice[32], bob[32], alice_public[32], bob_public[32];
    hex(alice, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    hex(bob, "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    x25519_base(alice_public, alice);
    show("x25519 RFC 7748 6.1 Alice", alice_public, 32);
    x25519_base(bob_public, bob);
    show("x25519 RFC 7748 6.1 Bob", bob_public, 32);
    x25519(out, alice, bob_public);
    show("x25519 RFC 7748 6.1 shared (Alice)", out, 32);
    x25519(out, bob, alice_public);
    show("x25519 RFC 7748 6.1 shared (Bob)", out, 32);

    for (int t = 0; t < 3; t++) {
        uint8_t seed[32], public_key[32], message[8];
        char name[48];
        hex(seed, ed25519_tests[t][0]);
        size_t n = hex(message, ed25519_tests[t][2]);
        ed25519_public(public_key, seed);
        snprintf(name, sizeof(name), "ed25519 TEST %d public", t + 1);
        show(name, public_key, 32);
        ed25519_sign(sig, seed, public_key, message, n);
        snprintf(name, sizeof(name), "ed25519 TEST %d signature", t + 1);
        show(name, sig, 64);
        printf("ed25519 TEST %d verify: %s\n", t + 1,
               ed25519_verify(sig, public_key, message, n) ? "yes" : "no");
    }
    /* Signature 3 again, spoiled four ways. */
    uint8_t seed[32], public_key[32], message[2] = { 0xaf, 0x82 }, bad[64];
    hex(seed, ed25519_tests[2][0]);
    hex(public_key, ed25519_tests[2][1]);
    ed25519_sign(sig, seed, public_key, message, 2);
    memcpy(bad, sig, 64);
    bad[10] ^= 1;
    printf("ed25519 a changed signature: %s\n",
           ed25519_verify(bad, public_key, message, 2) ? "accepted" : "rejected");
    message[1] ^= 1;
    printf("ed25519 a changed message: %s\n",
           ed25519_verify(sig, public_key, message, 2) ? "accepted" : "rejected");
    message[1] ^= 1;
    static const uint8_t l[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                   0xa2, 0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 0x10 };
    memcpy(bad, sig, 64);
    unsigned carry = 0;
    for (int i = 0; i < 32; i++) {                  /* S + L: the same point, too big */
        carry += bad[32 + i] + l[i];
        bad[32 + i] = (uint8_t)carry;
        carry >>= 8;
    }
    printf("ed25519 S + L: %s\n", ed25519_verify(bad, public_key, message, 2) ? "accepted" : "rejected");
    uint8_t not_a_point[32] = { 2 };                /* y = 2: no x for it */
    printf("ed25519 a key not on the curve: %s\n",
           ed25519_verify(sig, not_a_point, message, 2) ? "accepted" : "rejected");

    for (int i = 0; i < 4; i++) {
        char label[16], name[24];
        snprintf(label, sizeof(label), "seed %d", i);
        uint8_t derived[64], text[64];
        sha512(label, strlen(label), derived);
        snprintf(label, sizeof(label), "message %d", i);
        sha512(label, strlen(label), text);
        size_t n = 17 * (size_t)i;
        x25519(out, derived, derived + 32);
        snprintf(name, sizeof(name), "x25519 %d", i);
        show(name, out, 32);
        ed25519_public(public_key, derived);
        ed25519_sign(sig, derived, public_key, text, n);
        printf("ed25519 %d: ", i);
        for (int j = 0; j < 32; j++)
            printf("%02x", public_key[j]);
        printf(" ");
        for (int j = 0; j < 64; j++)
            printf("%02x", sig[j]);
        printf("\n");
        for (int j = 0; j < 5; j++)
            memcpy(data + j * n, text, n);
        poly1305(derived, data, 5 * n, h);
        snprintf(name, sizeof(name), "poly1305 %d", i);
        show(name, h, 16);
        for (int j = 0; j < 9; j++)
            memcpy(data + j * n, text, n);
        chacha20(derived, derived + 32, data, out, 9 * n);
        snprintf(name, sizeof(name), "chacha20 %d", i);
        show(name, out, 9 * n);
        for (int j = 0; j < 11; j++)
            memcpy(data + j * n, text, n);
        sha512(data, 11 * n, h);
        snprintf(name, sizeof(name), "sha512 %d", i);
        show(name, h, 64);
    }

    /* How long things take. */
    long start = milliseconds();
    for (int i = 0; i < 10; i++)
        x25519(out, alice, bob_public);
    long x = milliseconds() - start;
    start = milliseconds();
    for (int i = 0; i < 10; i++)
        ed25519_sign(sig, seed, public_key, message, 2);
    long sign = milliseconds() - start;
    start = milliseconds();
    for (int i = 0; i < 10; i++)
        ed25519_verify(sig, public_key, message, 2);
    long verify = milliseconds() - start;
    start = milliseconds();
    chacha20(key, block, big, big, sizeof(big));
    poly1305(key, big, sizeof(big), h);
    long bulk = milliseconds() - start;
    printf("time: x25519 %ld.%ld ms, ed25519 sign %ld.%ld ms, verify %ld.%ld ms; "
           "chacha20 and poly1305 over 1 MB %ld ms\n", x / 10, x % 10, sign / 10, sign % 10,
           verify / 10, verify % 10, bulk);
    return 0;
}
