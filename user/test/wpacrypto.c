/*
 * Test: Wi-Fi's cryptography (step 24a), inside RitterOS: the AES inverse
 * cipher (FIPS 197 C.1, C.3; four blocks at once), SHA-1 (FIPS 180's
 * examples), HMAC-SHA1 (RFC 2202 cases 1, 2, 6), PBKDF2-HMAC-SHA1 (RFC 6070;
 * IEEE 802.11 Annex J.4's passphrase-to-key cases), 802.11's PRF (Annex
 * J.3) and KDF-SHA256 (a value from scripts/test.sh's Python), AES key wrap
 * (RFC 3394 4.1, 4.3, 4.6; a tampered wrap refused), AES-CMAC (RFC 4493's
 * four examples), AES-CCM (RFC 3610 packet vector 1; a tampered tag
 * refused, the text then zeroed); SAE (24b): RFC 9380's SSWU map and point
 * sum for P-256, IEEE 802.11-2020 Annex J.10's hunting-and-pecking commit,
 * KCK, PMK and PMKID and its hash-to-element PWE (as hostapd's tests carry
 * them), refusals (a scalar out of range, an element off the curve, a
 * reflected commit, a wrong confirm), and two stations agreeing with random
 * numbers. The values the standards print were also checked against Python
 * (hashlib, hmac; SAE with plain big integers).
 */
#include <stdio.h>
#include <string.h>
#include "crypto.h"
#include "ritteros.h"

static int failures;

static void result(const char *name, int ok)
{
    printf("wpacrypto: %s: %s\n", name, ok ? "ok" : "FAILED");
    failures += !ok;
}

static int nibble(char c)
{
    return c <= '9' ? c - '0' : (c | 32) - 'a' + 10;
}

static size_t hex(const char *s, uint8_t *out)
{
    size_t n = 0;
    for (; s[0] && s[1]; s += 2)
        out[n++] = (uint8_t)(nibble(s[0]) << 4 | nibble(s[1]));
    return n;
}

static int same(const uint8_t *got, const char *want)
{
    uint8_t w[256];
    size_t n = hex(want, w);
    return memcmp(got, w, n) == 0;
}

static void test_aes_decrypt(void)
{
    uint8_t key[32], in[64], out[64], back[64];
    struct aes a;
    hex("000102030405060708090a0b0c0d0e0f", key);
    hex("69c4e0d86a7b0430d8cdb78070b4c55a", in);
    aes_init(&a, key, 16);
    aes_decrypt(&a, in, out);
    result("AES-128 decryption (FIPS 197 C.1)", same(out, "00112233445566778899aabbccddeeff"));
    hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key);
    hex("8ea2b7ca516745bfeafc49904b496089", in);
    aes_init(&a, key, 32);
    aes_decrypt(&a, in, out);
    result("AES-256 decryption (FIPS 197 C.3)", same(out, "00112233445566778899aabbccddeeff"));
    for (int i = 0; i < 64; i++)
        in[i] = (uint8_t)(i * 37 + 11);
    aes_encrypt4(&a, in, out);
    aes_decrypt4(&a, out, back);
    result("AES: four blocks there and back", memcmp(in, back, 64) == 0);
}

static void test_sha1(void)
{
    uint8_t h[SHA1_SIZE];
    static char million[1000000];
    sha1("abc", 3, h);
    int ok = same(h, "a9993e364706816aba3e25717850c26c9cd0d89d");
    sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
    ok &= same(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    memset(million, 'a', sizeof(million));
    struct sha1 s;
    sha1_init(&s);
    for (int i = 0; i < 1000; i++)                      /* in pieces, across blocks */
        sha1_update(&s, million + 1000 * i, 1000);
    sha1_final(&s, h);
    ok &= same(h, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    result("SHA-1 (FIPS 180: abc, 448 bits, a million a)", ok);
    uint8_t key[80];
    memset(key, 0x0b, 20);
    hmac_sha1(key, 20, "Hi There", 8, h);
    ok = same(h, "b617318655057264e28bc0b6fb378c8ef146be00");
    hmac_sha1("Jefe", 4, "what do ya want for nothing?", 28, h);
    ok &= same(h, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    memset(key, 0xaa, 80);
    hmac_sha1(key, 80, "Test Using Larger Than Block-Size Key - Hash Key First", 54, h);
    ok &= same(h, "aa4ae5e15272d00e95705637ce8a3b55ed402112");
    result("HMAC-SHA1 (RFC 2202 cases 1, 2, 6)", ok);
}

static void test_pbkdf2(void)
{
    uint8_t out[32];
    pbkdf2_sha1("password", 8, (const uint8_t *)"salt", 4, 1, out, 20);
    int ok = same(out, "0c60c80f961f0e71f3a9b524af6012062fe037a6");
    pbkdf2_sha1("password", 8, (const uint8_t *)"salt", 4, 4096, out, 20);
    ok &= same(out, "4b007901b765489abead49d926f721d065a429c1");
    pbkdf2_sha1("passwordPASSWORDpassword", 24,
                (const uint8_t *)"saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096, out, 25);
    ok &= same(out, "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038");
    result("PBKDF2-HMAC-SHA1 (RFC 6070)", ok);
    pbkdf2_sha1("password", 8, (const uint8_t *)"IEEE", 4, 4096, out, 32);
    ok = same(out, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
    pbkdf2_sha1("ThisIsAPassword", 15, (const uint8_t *)"ThisIsASSID", 11, 4096, out, 32);
    ok &= same(out, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af");
    result("WPA2 passphrase to key (802.11 Annex J.4)", ok);
}

static void test_kdf(void)
{
    uint8_t key[32], out[64], ctx[76];
    memset(key, 0x0b, 20);
    ieee80211_prf(key, 20, "prefix", "Hi There", 8, out, 64);
    int ok = same(out, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
                       "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a");
    ieee80211_prf("Jefe", 4, "prefix-2", "what do ya want for nothing?", 28, out, 64);
    ok &= same(out, "47c4908e30c947521ad20be9053450ecbea23d3aa604b77326d8b3825ff7475c"
                    "06f51fb9c5313d1e9f90d897d134b72e090fc23150bc8414382043418678e700");
    result("802.11 PRF (Annex J.3)", ok);
    for (int i = 0; i < 32; i++)
        key[i] = (uint8_t)i;
    for (int i = 0; i < 76; i++)
        ctx[i] = (uint8_t)(100 + i);
    ieee80211_kdf_sha256(key, 32, "Pairwise key expansion", ctx, 76, out, 48);
    result("802.11 KDF-SHA256", same(out, "f8bee50e6e9cfe26a3d56c3b7f7281597fdae9918790e8f1"
                                          "d4f3eb0911fa6b7b86eacabc7adecc4ae7d581c5acb15cf9"));
}

static void test_wrap(void)
{
    uint8_t kek[32], data[32], out[40], back[32];
    struct aes a;
    hex("000102030405060708090a0b0c0d0e0f", kek);
    hex("00112233445566778899aabbccddeeff", data);
    aes_init(&a, kek, 16);
    aes_key_wrap(&a, data, 16, out);
    int ok = same(out, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5");
    ok &= aes_key_unwrap(&a, out, 24, back) && memcmp(back, data, 16) == 0;
    hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", kek);
    aes_init(&a, kek, 32);
    aes_key_wrap(&a, data, 16, out);
    ok &= same(out, "64e8c3f9ce0f5ba263e9777905818a2a93c8191e7d6e8ae7");
    hex("00112233445566778899aabbccddeeff000102030405060708090a0b0c0d0e0f", data);
    aes_key_wrap(&a, data, 32, out);
    ok &= same(out, "28c9f404c4b810f4cbccb35cfb87f8263f5786e2d80ed326cbc7f0e71a99f43bfb988b9b7a02dd21");
    ok &= aes_key_unwrap(&a, out, 40, back) && memcmp(back, data, 32) == 0;
    result("AES key wrap (RFC 3394 4.1, 4.3, 4.6)", ok);
    out[20] ^= 1;
    result("AES key unwrap: a tampered wrap refused",
           !aes_key_unwrap(&a, out, 40, back) && back[0] == 0 && back[31] == 0);
}

static void test_cmac(void)
{
    uint8_t key[16], msg[64], tag[16];
    struct aes a;
    hex("2b7e151628aed2a6abf7158809cf4f3c", key);
    hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", msg);
    aes_init(&a, key, 16);
    aes_cmac(&a, msg, 0, tag);
    int ok = same(tag, "bb1d6929e95937287fa37d129b756746");
    aes_cmac(&a, msg, 16, tag);
    ok &= same(tag, "070a16b46b4d4144f79bdd9dd04a287c");
    aes_cmac(&a, msg, 40, tag);
    ok &= same(tag, "dfa66747de9ae63030ca32611497c827");
    aes_cmac(&a, msg, 64, tag);
    ok &= same(tag, "51f0bebf7e3b9d92fc49741779363cfe");
    result("AES-CMAC (RFC 4493: 0, 16, 40, 64 bytes)", ok);
}

static void test_ccm(void)
{
    uint8_t key[16], nonce[13], packet[31], out[23], back[23], tag[8];
    struct aes a;
    hex("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", key);
    hex("00000003020100a0a1a2a3a4a5", nonce);
    for (int i = 0; i < 31; i++)
        packet[i] = (uint8_t)i;
    aes_init(&a, key, 16);
    aes_ccm_seal(&a, nonce, packet, 8, packet + 8, out, 23, tag, 8);
    int ok = same(out, "588c979a61c663d2f066d0c2c0f989806d5f6b61dac384") &&
             same(tag, "17e8d12cfdf926e0");
    ok &= aes_ccm_open(&a, nonce, packet, 8, out, back, 23, tag, 8) &&
          memcmp(back, packet + 8, 23) == 0;
    result("AES-CCM (RFC 3610 packet vector 1)", ok);
    tag[7] ^= 0x80;
    result("AES-CCM: a tampered tag refused, nothing given out",
           !aes_ccm_open(&a, nonce, packet, 8, out, back, 23, tag, 8) && back[0] == 0 &&
           back[22] == 0);
}

static void test_sae(void)
{
    uint8_t u[32], q0[64], q1[64], sum[64], want[64];
    hex("ad5342c66a6dd0ff080df1da0ea1c04b96e0330dd89406465eeba11582515009", u);
    p256_sswu(u, q0);
    hex("ab640a12220d3ff283510ff3f4b1953d09fad35795140b1c5d64f313967934d5"
        "dccb558863804a881d4fff3455716c836cef230e5209594ddd33d85c565b19b1", want);
    int ok = memcmp(q0, want, 64) == 0;
    hex("8c0f1d43204bd6f6ea70ae8013070a1518b43873bcd850aafa0a9e220e2eea5a", u);
    p256_sswu(u, q1);
    hex("51cce63c50d972a6e51c61334f0f4875c9ac1cd2d3238412f84e31da7d980ef5"
        "b45d1a36d00ad90e5ec7840a60a4de411917fbe7c82c3949a6e699e5a1b66aac", want);
    ok &= memcmp(q1, want, 64) == 0;
    ok &= p256_add(q0, q1, sum);
    hex("2c15230b26dbc6fc9a37051158c95b79656e17a1a920b11394ca91c44247d3e4"
        "8a7a74985cc5c776cdfe4b1f19884970453912e9d31528c060be9ab5c43e8415", want);
    ok &= memcmp(sum, want, 64) == 0;
    result("P-256 SSWU map and sum (RFC 9380 J.1.1)", ok);

    static struct sae s, peer_s;
    uint8_t a[6], b[6], rand[32], mask[32], peer[98], commit[98];
    hex("4d3f2fffe387", a);
    hex("a5d8aa958e3c", b);
    hex("992465fd3daa3c60aa6565b7f62a2a7f2e12dd12f198faf4fbed89d7ff1ace94", rand);
    hex("9507a90f777a044d6a0830b91ea3d5dd70bece44e1acffb86983b5e1bf9fb322", mask);
    ok = sae_pwe_hunt(&s, a, b, "mekmitasdigoat", 14) && sae_commit(&s, rand, mask);
    commit[0] = 19, commit[1] = 0;
    memcpy(commit + 2, s.scalar, 32);
    memcpy(commit + 34, s.element, 64);
    ok &= same(commit, "13002e2c0f0db52440ad146d967114ce005ce1eab0aa2c2e5c2871b774f6c2575c"
                       "65d5ad9e00829707aa36ba8b859738fc961d08243505f47c035376d7ac4bc8d7"
                       "b95083bf43827d0fc31ed778dd3671fd21a46d1091d64b6f9a1e1272621325dbe1");
    result("SAE hunting and pecking: the commit (802.11 Annex J.10)", ok);
    hex("1300591b96f3397fb945100848e7b550543b6720d88337ee93fc49fd6df7e08b5223e71b9bb048d3"
        "873f20556953a96c91536fd8ee6ca9b4a68a148b056a909be03e83ae208f60f8ef5537858074db06"
        "687032399862999b511e0a1552a5fea317c2", peer);
    ok = sae_process(&s, peer + 2, peer + 34);
    ok &= same(s.kck, "1e733f6d9bd53256287304338831b09a39406d121017073a5c30db36f36cb81a");
    ok &= same(s.pmk, "4e4dfab1a2dd8ac1a91790f953faaa452ae5c6873ab75b63605ba663f8a7fe59");
    ok &= same(s.pmkid, "8747a600eea3f9f22475df58ca1e5498");
    result("SAE: KCK, PMK, PMKID (802.11 Annex J.10)", ok);

    uint8_t pt[64];
    sae_pt(pt, "byteme", 6, "mekmitasdigoat", 14, "psk4internet", 12);
    hex("00095b66ec1e", a);
    hex("000b6bd90246", b);
    ok = sae_pwe_from_pt(&s, pt, a, b);
    ok &= same(s.pwe, "c93049b9e64000f848201649e999f2b5c22dea69b5632c9df4d633b8aa1f6c1e"
                      "73634e94b53d82e7383a8d258199d9dc1a5ee8269d060382ccbf33e614ff59a0");
    result("SAE hash to element: the PWE (802.11 Annex J.10)", ok);

    /* Two stations, random numbers, hash to element. */
    hex("020000000001", a);
    hex("020000000002", b);
    sae_pt(pt, "RitterNet", 9, "correct horse", 13, "", 0);
    ok = sae_pwe_from_pt(&s, pt, a, b) && sae_pwe_from_pt(&peer_s, pt, b, a) &&
         sae_commit(&s, 0, 0) && sae_commit(&peer_s, 0, 0) &&
         sae_process(&s, peer_s.scalar, peer_s.element) &&
         sae_process(&peer_s, s.scalar, s.element);
    uint8_t c1[32], c2[32];
    sae_confirm(&s, 1, c1);
    sae_confirm(&peer_s, 1, c2);
    ok &= sae_check_confirm(&peer_s, 1, c1) && sae_check_confirm(&s, 1, c2) &&
          memcmp(s.pmk, peer_s.pmk, 32) == 0 && memcmp(s.pmkid, peer_s.pmkid, 16) == 0;
    result("SAE: two stations agree (random numbers, both confirms)", ok);
    c1[5] ^= 1;
    uint8_t bad[64], zero32[32] = { 0 };
    memcpy(bad, peer_s.element, 64);
    bad[63] ^= 1;
    static struct sae t;
    t = s;
    ok = !sae_check_confirm(&peer_s, 1, c1) && !sae_check_confirm(&peer_s, 2, c2);
    ok &= !sae_process(&t, peer_s.scalar, bad);
    t = s;
    ok &= !sae_process(&t, zero32, peer_s.element);
    t = s;
    ok &= !sae_process(&t, s.scalar, s.element);
    result("SAE: refused (a wrong confirm, an element off the curve, scalar 0, a reflection)", ok);
    sae_pt(pt, "RitterNet", 9, "correct horsf", 13, "", 0);
    ok = sae_pwe_from_pt(&peer_s, pt, b, a) && sae_commit(&peer_s, 0, 0) &&
         sae_commit(&s, 0, 0) && sae_process(&s, peer_s.scalar, peer_s.element) &&
         sae_process(&peer_s, s.scalar, s.element);
    sae_confirm(&peer_s, 1, c2);
    result("SAE: another password, the confirm does not check",
           ok && !sae_check_confirm(&s, 1, c2) && memcmp(s.pmk, peer_s.pmk, 32) != 0);
}

int main(void)
{
    test_aes_decrypt();
    test_sha1();
    test_pbkdf2();
    test_kdf();
    test_wrap();
    test_cmac();
    test_ccm();
    test_sae();
    printf("wpacrypto: %s\n", failures ? "FAILED" : "all ok");
    return failures != 0;
}
