/*
 * netlibs (46e): one call into each of RitterOS's system libraries from
 * upstream, with what it found (the SDK's example of using them):
 *   netlibs     the libraries' versions; SHA-256 by OpenSSL, CRC-32 by
 *               zlib, a round trip through brotli, an HPACK-free check of
 *               nghttp2's version, a registrable domain by libpsl, a URL
 *               escaped by libcurl
 */
#include <brotli/decode.h>
#include <brotli/encode.h>
#include <curl/curl.h>
#include <libpsl.h>
#include <nghttp2/nghttp2.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

int main(void)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    EVP_Digest("abc", 3, md, &n, EVP_sha256(), NULL);
    printf("netlibs: %s, sha256(abc) begins %02x%02x%02x%02x\n", OpenSSL_version(OPENSSL_VERSION), md[0], md[1],
           md[2], md[3]);

    printf("netlibs: zlib %s, crc32(123456789) %08lx\n", zlibVersion(),
           crc32(0, (const Bytef *)"123456789", 9));

    const char *text = "brotli, brotli, brotli and more brotli";
    uint8_t z[128], back[128];
    size_t zn = sizeof(z), bn = sizeof(back);
    int ok = BrotliEncoderCompress(9, 22, BROTLI_MODE_TEXT, strlen(text), (const uint8_t *)text, &zn, z) &&
             BrotliDecoderDecompress(zn, z, &bn, back) == BROTLI_DECODER_RESULT_SUCCESS && bn == strlen(text) &&
             memcmp(back, text, bn) == 0;
    uint32_t v = BrotliDecoderVersion();
    printf("netlibs: brotli %u.%u.%u, a round trip %s\n", v >> 24, (v >> 12) & 0xFFF, v & 0xFFF, ok ? "ok" : "FAILED");

    printf("netlibs: nghttp2 %s\n", nghttp2_version(0)->version_str);

    const char *domain = psl_registrable_domain(psl_builtin(), "www.example.co.uk");
    printf("netlibs: libpsl %s, the registrable domain of www.example.co.uk is %s\n", psl_get_version(),
           domain ? domain : "(none)");

    CURL *e = curl_easy_init();
    char *escaped = curl_easy_escape(e, "a b&c", 0);
    printf("netlibs: libcurl %s, escaped \"a b&c\" as %s\n", curl_version_info(CURLVERSION_NOW)->version, escaped);
    curl_free(escaped);
    curl_easy_cleanup(e);
    return 0;
}
