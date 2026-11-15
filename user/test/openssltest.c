/*
 * Test: (46b) OpenSSL, the system libraries libcrypto.so.3 and libssl.so.3
 * built from upstream's sources. Each line says ok or what failed; the
 * last is "all ok" or "FAILED".
 *   openssltest     the version; known answers from the standards for what
 *                   WebKit's WebCrypto uses (SHA-1 and SHA-2, HMAC, HKDF,
 *                   PBKDF2, AES-ECB/CBC/CTR/GCM/KW, Ed25519, X25519); round
 *                   trips with keys made here (RSA-OAEP, RSA-PSS, RSA
 *                   PKCS#1 v1.5, ECDSA and ECDH on P-256, P-384 and P-521),
 *                   each spoiled signature refused; 1 MiB through
 *                   AES-256-GCM and ChaCha20-Poly1305 and back, a changed
 *                   byte refused; the random generator in a forked child
 *                   (other bytes than the parent's); eight threads hashing
 *                   and drawing random bytes at once; the default CA file
 *                   and its roots.
 */
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define T "openssltest: "

static int failures;

static void check(const char *what, int ok)
{
    printf(T "%s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        failures++;
        ERR_print_errors_fp(stdout);
    }
}

static size_t unhex(const char *h, unsigned char *out)
{
    size_t n = 0;
    for (; h[0] && h[1]; h += 2) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[n++] = (unsigned char)v;
    }
    return n;
}

static int same_hex(const unsigned char *got, size_t n, const char *want)
{
    char text[1024];
    if (2 * n + 1 > sizeof(text) || strlen(want) != 2 * n)
        return 0;
    for (size_t i = 0; i < n; i++)
        snprintf(text + 2 * i, 3, "%02x", got[i]);
    if (strcmp(text, want) != 0)
        printf(T "  got %s\n" T "  not %s\n", text, want);
    return strcmp(text, want) == 0;
}

static void digests(void)
{
    static const struct { const char *name, *want; } d[] = {
        { "SHA1", "a9993e364706816aba3e25717850c26c9cd0d89d" },
        { "SHA224", "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7" },
        { "SHA256", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "SHA384", "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
                    "8086072ba1e7cc2358baeca134c825a7" },
        { "SHA512", "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f" },
    };
    for (size_t i = 0; i < sizeof(d) / sizeof(d[0]); i++) {
        unsigned char md[64];
        unsigned int n = 0;
        int ok = EVP_Digest("abc", 3, md, &n, EVP_get_digestbyname(d[i].name), 0) == 1;
        char what[64];
        snprintf(what, sizeof(what), "%s of \"abc\" (FIPS 180-4)", d[i].name);
        check(what, ok && same_hex(md, n, d[i].want));
    }

    unsigned char key[20], out[64];
    memset(key, 0x0b, sizeof(key));
    unsigned int n = 0;
    HMAC(EVP_sha256(), key, 20, (const unsigned char *)"Hi There", 8, out, &n);
    check("HMAC-SHA256 (RFC 4231, 1)",
          same_hex(out, n, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));

    unsigned char ikm[22], salt[13], info[10], okm[42];
    memset(ikm, 0x0b, sizeof(ikm));
    for (int i = 0; i < 13; i++)
        salt[i] = (unsigned char)i;
    for (int i = 0; i < 10; i++)
        info[i] = (unsigned char)(0xf0 + i);
    EVP_PKEY_CTX *k = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, 0);
    size_t okm_len = sizeof(okm);
    int ok = k && EVP_PKEY_derive_init(k) == 1 && EVP_PKEY_CTX_set_hkdf_md(k, EVP_sha256()) == 1 &&
             EVP_PKEY_CTX_set1_hkdf_salt(k, salt, 13) == 1 && EVP_PKEY_CTX_set1_hkdf_key(k, ikm, 22) == 1 &&
             EVP_PKEY_CTX_add1_hkdf_info(k, info, 10) == 1 && EVP_PKEY_derive(k, okm, &okm_len) == 1;
    EVP_PKEY_CTX_free(k);
    check("HKDF-SHA256 (RFC 5869, 1)", ok && same_hex(okm, okm_len, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90"
                                                                   "cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));

    ok = PKCS5_PBKDF2_HMAC_SHA1("password", 8, (const unsigned char *)"salt", 4, 1, 20, out) == 1 &&
         same_hex(out, 20, "0c60c80f961f0e71f3a9b524af6012062fe037a6") &&
         PKCS5_PBKDF2_HMAC_SHA1("password", 8, (const unsigned char *)"salt", 4, 4096, 20, out) == 1 &&
         same_hex(out, 20, "4b007901b765489abead49d926f721d065a429c1");
    check("PBKDF2-HMAC-SHA1, 1 and 4096 rounds (RFC 6070)", ok);
}

/* One block of a cipher in one direction; its output's length. */
static int crypt_block(const EVP_CIPHER *c, const char *key, const char *iv, const char *in, unsigned char *out,
                 int encrypt)
{
    unsigned char k[32], v[16], p[64];
    unhex(key, k);
    if (iv)
        unhex(iv, v);
    int n = (int)unhex(in, p), len = 0, more = 0;
    EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_set_flags(x, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
    int ok = EVP_CipherInit_ex(x, c, 0, k, iv ? v : 0, encrypt) == 1 && EVP_CIPHER_CTX_set_padding(x, 0) == 1 &&
             EVP_CipherUpdate(x, out, &len, p, n) == 1 && EVP_CipherFinal_ex(x, out + len, &more) == 1;
    EVP_CIPHER_CTX_free(x);
    return ok ? len + more : -1;
}

static void ciphers(void)
{
    unsigned char out[64];
    int n = crypt_block(EVP_aes_128_ecb(), "000102030405060708090a0b0c0d0e0f", 0, "00112233445566778899aabbccddeeff",
                  out, 1);
    check("AES-128 (FIPS 197, C.1)", n == 16 && same_hex(out, 16, "69c4e0d86a7b0430d8cdb78070b4c55a"));
    n = crypt_block(EVP_aes_128_cbc(), "2b7e151628aed2a6abf7158809cf4f3c", "000102030405060708090a0b0c0d0e0f",
              "6bc1bee22e409f96e93d7e117393172a", out, 1);
    check("AES-128-CBC (SP 800-38A, F.2.1)", n == 16 && same_hex(out, 16, "7649abac8119b246cee98e9b12e9197d"));
    n = crypt_block(EVP_aes_128_ctr(), "2b7e151628aed2a6abf7158809cf4f3c", "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
              "6bc1bee22e409f96e93d7e117393172a", out, 1);
    check("AES-128-CTR (SP 800-38A, F.5.1)", n == 16 && same_hex(out, 16, "874d6191b620e3261bef6864990db6ce"));
    n = crypt_block(EVP_aes_128_wrap(), "000102030405060708090a0b0c0d0e0f", 0, "00112233445566778899aabbccddeeff",
              out, 1);
    check("AES-128 key wrap (RFC 3394, 4.1)",
          n == 24 && same_hex(out, 24, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"));
    n = crypt_block(EVP_aes_128_wrap(), "000102030405060708090a0b0c0d0e0f", 0,
              "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", out, 0);
    check("AES-128 key unwrap (RFC 3394, 4.1)", n == 16 && same_hex(out, 16, "00112233445566778899aabbccddeeff"));

    /* AES-128-GCM, the GCM specification's test case 2: zero key, IV and block. */
    unsigned char zero[16] = { 0 }, ct[16], tag[16];
    int len = 0, more = 0;
    EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new();
    int ok = EVP_EncryptInit_ex(x, EVP_aes_128_gcm(), 0, zero, zero) == 1 &&
             EVP_EncryptUpdate(x, ct, &len, zero, 16) == 1 && EVP_EncryptFinal_ex(x, ct + len, &more) == 1 &&
             EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
    EVP_CIPHER_CTX_free(x);
    check("AES-128-GCM (the GCM specification, test case 2)",
          ok && same_hex(ct, 16, "0388dace60b6a392f328c2b971b2fe78") &&
          same_hex(tag, 16, "ab6e47d42cec13bdf53a67b21257bddf"));
}

/* 1 MiB through an AEAD and back; then the same with one byte changed. */
static void aead_round_trip(const char *name, const EVP_CIPHER *c)
{
    size_t n = 1 << 20;
    unsigned char *plain = malloc(n), *sealed = malloc(n + 16), *opened = malloc(n + 16);
    unsigned char key[32], iv[12], tag[16];
    RAND_bytes(key, sizeof(key));
    RAND_bytes(iv, sizeof(iv));
    for (size_t i = 0; i < n; i++)
        plain[i] = (unsigned char)(i * 31 + (i >> 9));
    int len = 0, more = 0;
    EVP_CIPHER_CTX *x = EVP_CIPHER_CTX_new();
    int ok = EVP_EncryptInit_ex(x, c, 0, key, iv) == 1 && EVP_EncryptUpdate(x, sealed, &len, plain, (int)n) == 1 &&
             EVP_EncryptFinal_ex(x, sealed + len, &more) == 1 &&
             EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_AEAD_GET_TAG, 16, tag) == 1;
    for (int spoil = 0; spoil < 2; spoil++) {
        if (spoil)
            sealed[n / 3] ^= 1;
        int got = EVP_DecryptInit_ex(x, c, 0, key, iv) == 1 &&
                  EVP_DecryptUpdate(x, opened, &len, sealed, (int)n) == 1 &&
                  EVP_CIPHER_CTX_ctrl(x, EVP_CTRL_AEAD_SET_TAG, 16, tag) == 1 &&
                  EVP_DecryptFinal_ex(x, opened + len, &more) == 1;
        ok = ok && (spoil ? !got : got && memcmp(plain, opened, n) == 0);
    }
    ERR_clear_error();
    EVP_CIPHER_CTX_free(x);
    char what[96];
    snprintf(what, sizeof(what), "%s: 1 MiB sealed and opened, a changed byte refused", name);
    check(what, ok);
    free(plain);
    free(sealed);
    free(opened);
}

static void curves25519(void)
{
    unsigned char secret[32], pub[32], sig[64];
    unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", secret);
    EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, 0, secret, 32);
    size_t n = sizeof(pub), slen = sizeof(sig);
    EVP_MD_CTX *m = EVP_MD_CTX_new();
    int ok = k && EVP_PKEY_get_raw_public_key(k, pub, &n) == 1 &&
             same_hex(pub, 32, "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a") &&
             EVP_DigestSignInit(m, 0, 0, 0, k) == 1 && EVP_DigestSign(m, sig, &slen, (const unsigned char *)"", 0) == 1 &&
             same_hex(sig, 64, "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                               "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    EVP_MD_CTX_free(m);
    m = EVP_MD_CTX_new();
    ok = ok && EVP_DigestVerifyInit(m, 0, 0, 0, k) == 1 &&
         EVP_DigestVerify(m, sig, 64, (const unsigned char *)"", 0) == 1;
    sig[10] ^= 4;
    EVP_MD_CTX_free(m);
    m = EVP_MD_CTX_new();
    ok = ok && EVP_DigestVerifyInit(m, 0, 0, 0, k) == 1 &&
         EVP_DigestVerify(m, sig, 64, (const unsigned char *)"", 0) != 1;
    ERR_clear_error();
    EVP_MD_CTX_free(m);
    EVP_PKEY_free(k);
    check("Ed25519 (RFC 8032, test 1), a spoiled signature refused", ok);

    unsigned char a[32], bpub[32], shared[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a);
    unhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", bpub);
    EVP_PKEY *alice = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, 0, a, 32);
    EVP_PKEY *bob = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, 0, bpub, 32);
    EVP_PKEY_CTX *d = EVP_PKEY_CTX_new(alice, 0);
    n = sizeof(shared);
    ok = d && EVP_PKEY_derive_init(d) == 1 && EVP_PKEY_derive_set_peer(d, bob) == 1 &&
         EVP_PKEY_derive(d, shared, &n) == 1 &&
         same_hex(shared, 32, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    EVP_PKEY_CTX_free(d);
    EVP_PKEY_free(alice);
    EVP_PKEY_free(bob);
    check("X25519 (RFC 7748, 6.1)", ok);
}

/* A signature with key k (padding for RSA: 0 none to set), verified, then
 * refused once one byte of it is changed. */
static int sign_and_verify(EVP_PKEY *k, int padding)
{
    const unsigned char msg[] = "the message to sign";
    unsigned char sig[1024];
    size_t n = sizeof(sig);
    EVP_MD_CTX *m = EVP_MD_CTX_new();
    EVP_PKEY_CTX *p = 0;
    int ok = EVP_DigestSignInit(m, &p, EVP_sha256(), 0, k) == 1 &&
             (!padding || EVP_PKEY_CTX_set_rsa_padding(p, padding) == 1) &&
             EVP_DigestSign(m, sig, &n, msg, sizeof(msg)) == 1;
    for (int spoil = 0; ok && spoil < 2; spoil++) {
        if (spoil)
            sig[n / 2] ^= 0x10;
        EVP_MD_CTX_free(m);
        m = EVP_MD_CTX_new();
        int good = EVP_DigestVerifyInit(m, &p, EVP_sha256(), 0, k) == 1 &&
                   (!padding || EVP_PKEY_CTX_set_rsa_padding(p, padding) == 1) &&
                   EVP_DigestVerify(m, sig, n, msg, sizeof(msg)) == 1;
        ok = spoil ? !good : good;
    }
    ERR_clear_error();
    EVP_MD_CTX_free(m);
    return ok;
}

static void rsa(void)
{
    EVP_PKEY *k = EVP_PKEY_Q_keygen(0, 0, "RSA", (size_t)2048);
    check("an RSA 2048 key made", k != 0);
    if (!k)
        return;
    unsigned char secret[32], sealed[256], opened[256];
    RAND_bytes(secret, sizeof(secret));
    size_t n = sizeof(sealed), m = sizeof(opened);
    EVP_PKEY_CTX *c = EVP_PKEY_CTX_new(k, 0);
    int ok = EVP_PKEY_encrypt_init(c) == 1 && EVP_PKEY_CTX_set_rsa_padding(c, RSA_PKCS1_OAEP_PADDING) == 1 &&
             EVP_PKEY_CTX_set_rsa_oaep_md(c, EVP_sha256()) == 1 &&
             EVP_PKEY_encrypt(c, sealed, &n, secret, sizeof(secret)) == 1 &&
             EVP_PKEY_decrypt_init(c) == 1 && EVP_PKEY_CTX_set_rsa_padding(c, RSA_PKCS1_OAEP_PADDING) == 1 &&
             EVP_PKEY_CTX_set_rsa_oaep_md(c, EVP_sha256()) == 1 &&
             EVP_PKEY_decrypt(c, opened, &m, sealed, n) == 1 && m == sizeof(secret) &&
             memcmp(opened, secret, m) == 0;
    sealed[7] ^= 1;
    m = sizeof(opened);
    ok = ok && EVP_PKEY_decrypt(c, opened, &m, sealed, n) != 1;
    ERR_clear_error();
    EVP_PKEY_CTX_free(c);
    check("RSA-OAEP (SHA-256): sealed and opened, a changed byte refused", ok);
    check("RSA-PSS (SHA-256): signed and verified, a spoiled signature refused",
          sign_and_verify(k, RSA_PKCS1_PSS_PADDING));
    check("RSA PKCS#1 v1.5 (SHA-256): signed and verified, a spoiled signature refused",
          sign_and_verify(k, RSA_PKCS1_PADDING));
    EVP_PKEY_free(k);
}

static void ec(void)
{
    static const char *const curves[] = { "P-256", "P-384", "P-521" };
    for (int i = 0; i < 3; i++) {
        EVP_PKEY *a = EVP_PKEY_Q_keygen(0, 0, "EC", curves[i]);
        EVP_PKEY *b = EVP_PKEY_Q_keygen(0, 0, "EC", curves[i]);
        char what[96];
        snprintf(what, sizeof(what), "ECDSA %s (SHA-256): signed and verified, a spoiled signature refused",
                 curves[i]);
        check(what, a && b && sign_and_verify(a, 0));
        unsigned char s1[80], s2[80];
        size_t n1 = sizeof(s1), n2 = sizeof(s2);
        EVP_PKEY_CTX *x = EVP_PKEY_CTX_new(a, 0), *y = EVP_PKEY_CTX_new(b, 0);
        int ok = x && y && EVP_PKEY_derive_init(x) == 1 && EVP_PKEY_derive_set_peer(x, b) == 1 &&
                 EVP_PKEY_derive(x, s1, &n1) == 1 && EVP_PKEY_derive_init(y) == 1 &&
                 EVP_PKEY_derive_set_peer(y, a) == 1 && EVP_PKEY_derive(y, s2, &n2) == 1 &&
                 n1 == n2 && memcmp(s1, s2, n1) == 0;
        snprintf(what, sizeof(what), "ECDH %s: both sides derive the same secret", curves[i]);
        check(what, ok);
        EVP_PKEY_CTX_free(x);
        EVP_PKEY_CTX_free(y);
        EVP_PKEY_free(a);
        EVP_PKEY_free(b);
    }
}

/* The generator in a forked child must not repeat the parent's bytes. */
static void fork_random(void)
{
    unsigned char before[16], parent[16], child[16];
    int fds[2];
    if (RAND_bytes(before, 16) != 1 || pipe(fds) < 0) {
        check("random bytes after a fork differ from the parent's", 0);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        RAND_bytes(child, 16);
        _exit(write(fds[1], child, 16) == 16 ? 0 : 1);
    }
    RAND_bytes(parent, 16);
    int status = 0;
    int got = (int)read(fds[0], child, 16);
    waitpid(pid, &status, 0);
    close(fds[0]);
    close(fds[1]);
    check("random bytes after a fork differ from the parent's",
          pid > 0 && got == 16 && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
          memcmp(child, parent, 16) != 0 && memcmp(child, before, 16) != 0);
}

static void *hash_and_draw(void *arg)
{
    unsigned char buf[4096], md[32], want[32], r[32];
    memset(buf, (int)(size_t)arg, sizeof(buf));
    unsigned int n;
    EVP_Digest(buf, sizeof(buf), want, &n, EVP_sha256(), 0);
    for (int i = 0; i < 200; i++) {
        if (EVP_Digest(buf, sizeof(buf), md, &n, EVP_sha256(), 0) != 1 || memcmp(md, want, 32) != 0 ||
            RAND_bytes(r, sizeof(r)) != 1)
            return (void *)1;
    }
    return 0;
}

static void threads(void)
{
    pthread_t t[8];
    int ok = 1;
    for (size_t i = 0; i < 8; i++)
        ok = ok && pthread_create(&t[i], 0, hash_and_draw, (void *)(i + 1)) == 0;
    for (size_t i = 0; i < 8; i++) {
        void *r = (void *)1;
        pthread_join(t[i], &r);
        ok = ok && r == 0;
    }
    check("eight threads hashing and drawing random bytes at once", ok);
}

static void default_paths(void)
{
    check("the default CA file is /system/config/openssl/cert.pem",
          strcmp(X509_get_default_cert_file(), "/system/config/openssl/cert.pem") == 0);
    SSL_CTX *c = SSL_CTX_new(TLS_client_method());
    int ok = c && SSL_CTX_set_default_verify_paths(c) == 1;
    int roots = ok ? sk_X509_OBJECT_num(X509_STORE_get0_objects(SSL_CTX_get_cert_store(c))) : 0;
    char what[96];
    snprintf(what, sizeof(what), "the default verify paths load the system's roots (more than 100)");
    check(what, roots > 100);
    SSL_CTX_free(c);
}

int main(void)
{
    check("version 3.5.9", OPENSSL_VERSION_MAJOR == 3 && OPENSSL_VERSION_MINOR == 5 &&
                           strncmp(OpenSSL_version(OPENSSL_VERSION), "OpenSSL 3.5.9 ", 14) == 0);
    digests();
    ciphers();
    aead_round_trip("AES-256-GCM", EVP_aes_256_gcm());
    aead_round_trip("ChaCha20-Poly1305", EVP_chacha20_poly1305());
    curves25519();
    rsa();
    ec();
    fork_random();
    threads();
    default_paths();
    printf(T "%s\n", failures ? "FAILED" : "all ok");
    return failures != 0;
}
