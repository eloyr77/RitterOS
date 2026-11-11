/*
 * cctest's intrinsics part (32d), written by scripts/cctest_simd.py from
 * user/bin/cc/include/immintrin.h (do not edit: run it again): every
 * intrinsic on fixed vectors, each result in hex. clang's build and cc's
 * must print the same; on a processor without AVX2 and FMA both say so.
 */
#if defined(__x86_64__)
#include <immintrin.h>

#define SIMD __attribute__((target("avx2,fma")))

static __m256i i256[3];
static __m256 f256[3];
static __m256d d256[3];
static __m128i i128[3];
static __m128 f128[3];
static __m128d d128[3];
static unsigned char buffer[96] __attribute__((aligned(32)));

static void show(const char *name, const void *p, int n)
{
    const unsigned char *b = p;
    printf("intrinsics: %s:", name);
    for (int i = 0; i < n; i++)
        printf("%s%02x", i % 8 ? "" : " ", b[i]);
    printf("\n");
}

static void fill(void)
{
    for (int i = 0; i < (int)sizeof(buffer); i++)
        buffer[i] = (unsigned char)(i * 37 + 5);
}

SIMD static void simd_values(void)
{
    static const float fs[8] = { 1.5f, -2.25f, 1e30f, 0.0f, -0.0f, 3.0f, 0.1f, -7.75f };
    static const double ds[4] = { 2.5, -1e-300, 0.75, -3.0 };
    for (int k = 0; k < 3; k++) {
        unsigned char raw[32];
        float f[8];
        double d[4];
        for (int i = 0; i < 32; i++)
            raw[i] = (unsigned char)(i * 29 + k * 101 + (i & 1 ? 0x80 : 3));
        for (int i = 0; i < 8; i++)
            f[i] = fs[(i + k) % 8] * (float)(k + 1);
        for (int i = 0; i < 4; i++)
            d[i] = ds[(i + k) % 4] * (k + 1);
        memcpy(&i256[k], raw, 32);
        memcpy(&i128[k], raw + 8, 16);
        memcpy(&f256[k], f, 32);
        memcpy(&f128[k], f + 2, 16);
        memcpy(&d256[k], d, 32);
        memcpy(&d128[k], d + 1, 16);
    }
}

SIMD static void simd_calls(void)
{
    { __m256i __v = _mm256_setzero_si256(); show("_mm256_setzero_si256#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_setzero_ps(); show("_mm256_setzero_ps#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_setzero_pd(); show("_mm256_setzero_pd#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_set1_epi8((char)-91); show("_mm256_set1_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_set_epi8((char)-91, (char)-43, (char)5, (char)53, (char)101, (char)-107, (char)-59, (char)-11, (char)37, (char)85, (char)-123, (char)-75, (char)-27, (char)21, (char)69, (char)117, (char)-91, (char)-43, (char)5, (char)53, (char)101, (char)-107, (char)-59, (char)-11, (char)37, (char)85, (char)-123, (char)-75, (char)-27, (char)21, (char)69, (char)117); show("_mm256_set_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_setr_epi8((char)-91, (char)-43, (char)5, (char)53, (char)101, (char)-107, (char)-59, (char)-11, (char)37, (char)85, (char)-123, (char)-75, (char)-27, (char)21, (char)69, (char)117, (char)-91, (char)-43, (char)5, (char)53, (char)101, (char)-107, (char)-59, (char)-11, (char)37, (char)85, (char)-123, (char)-75, (char)-27, (char)21, (char)69, (char)117); show("_mm256_setr_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_set1_epi16((short)-28669); show("_mm256_set1_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_set_epi16((short)-28669, (short)-24563, (short)-20457, (short)-16351, (short)-12245, (short)-8139, (short)-4033, (short)73, (short)4179, (short)8285, (short)12391, (short)16497, (short)20603, (short)24709, (short)28815, (short)-32615); show("_mm256_set_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_setr_epi16((short)-28669, (short)-24563, (short)-20457, (short)-16351, (short)-12245, (short)-8139, (short)-4033, (short)73, (short)4179, (short)8285, (short)12391, (short)16497, (short)20603, (short)24709, (short)28815, (short)-32615); show("_mm256_setr_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_set1_epi32(3); show("_mm256_set1_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_set_epi32(3, -99978, 42, -99939, 81, -99900, 120, -99861); show("_mm256_set_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_setr_epi32(3, -99978, 42, -99939, 81, -99900, 120, -99861); show("_mm256_setr_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_set1_epi64x(-999999993LL); show("_mm256_set1_epi64x#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_set_epi64x(-999999993LL, 27LL, 1000000047LL, -1999999934LL); show("_mm256_set_epi64x#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_setr_epi64x(-999999993LL, 27LL, 1000000047LL, -1999999934LL); show("_mm256_setr_epi64x#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_set1_ps(-1.750f); show("_mm256_set1_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_set_ps(-1.750f, 0.500f, 2.750f, 5.000f, 7.250f, 9.500f, 11.750f, 14.000f); show("_mm256_set_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_setr_ps(-1.750f, 0.500f, 2.750f, 5.000f, 7.250f, 9.500f, 11.750f, 14.000f); show("_mm256_setr_ps#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_set1_pd(-4.500); show("_mm256_set1_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_set_pd(-4.500, -1.000, 2.500, 6.000); show("_mm256_set_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_setr_pd(-4.500, -1.000, 2.500, 6.000); show("_mm256_setr_pd#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_set_m128i(i128[0], i128[1]); show("_mm256_set_m128i#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_set_m128i(i128[1], i128[2]); show("_mm256_set_m128i#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_setr_m128i(i128[0], i128[1]); show("_mm256_setr_m128i#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_setr_m128i(i128[1], i128[2]); show("_mm256_setr_m128i#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_set_m128(f128[0], f128[1]); show("_mm256_set_m128#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_set_m128(f128[1], f128[2]); show("_mm256_set_m128#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_set_m128d(d128[0], d128[1]); show("_mm256_set_m128d#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_set_m128d(d128[1], d128[2]); show("_mm256_set_m128d#1", &__v, sizeof(__v)); }
    fill();
    { __m256i __v = _mm256_load_si256((__m256i *)buffer); show("_mm256_load_si256", &__v, sizeof(__v)); }
    fill();
    { __m256i __v = _mm256_loadu_si256((__m256i *)buffer); show("_mm256_loadu_si256", &__v, sizeof(__v)); }
    fill(); _mm256_store_si256((__m256i *)buffer, i256[1]); show("_mm256_store_si256", buffer, 64);
    fill(); _mm256_storeu_si256((__m256i *)buffer, i256[1]); show("_mm256_storeu_si256", buffer, 64);
    fill(); _mm256_stream_si256((__m256i *)buffer, i256[1]); show("_mm256_stream_si256", buffer, 64);
    fill();
    { __m256 __v = _mm256_load_ps((float *)buffer); show("_mm256_load_ps", &__v, sizeof(__v)); }
    fill();
    { __m256 __v = _mm256_loadu_ps((float *)buffer); show("_mm256_loadu_ps", &__v, sizeof(__v)); }
    fill(); _mm256_store_ps((float *)buffer, f256[1]); show("_mm256_store_ps", buffer, 64);
    fill(); _mm256_storeu_ps((float *)buffer, f256[1]); show("_mm256_storeu_ps", buffer, 64);
    fill(); _mm256_stream_ps((float *)buffer, f256[1]); show("_mm256_stream_ps", buffer, 64);
    fill();
    { __m256d __v = _mm256_load_pd((double *)buffer); show("_mm256_load_pd", &__v, sizeof(__v)); }
    fill();
    { __m256d __v = _mm256_loadu_pd((double *)buffer); show("_mm256_loadu_pd", &__v, sizeof(__v)); }
    fill(); _mm256_store_pd((double *)buffer, d256[1]); show("_mm256_store_pd", buffer, 64);
    fill(); _mm256_storeu_pd((double *)buffer, d256[1]); show("_mm256_storeu_pd", buffer, 64);
    fill(); _mm256_stream_pd((double *)buffer, d256[1]); show("_mm256_stream_pd", buffer, 64);
    fill();
    { __m256i __v = _mm256_lddqu_si256((__m256i *)buffer); show("_mm256_lddqu_si256", &__v, sizeof(__v)); }
    fill();
    { __m256i __v = _mm256_stream_load_si256((__m256i *)buffer); show("_mm256_stream_load_si256", &__v, sizeof(__v)); }
    fill();
    { __m256i __v = _mm256_maskload_epi32((int *)buffer, i256[1]); show("_mm256_maskload_epi32", &__v, sizeof(__v)); }
    fill(); _mm256_maskstore_epi32((int *)buffer, i256[1], i256[2]); show("_mm256_maskstore_epi32", buffer, 64);
    fill();
    { __m128i __v = _mm_maskload_epi32((int *)buffer, i128[1]); show("_mm_maskload_epi32", &__v, sizeof(__v)); }
    fill(); _mm_maskstore_epi32((int *)buffer, i128[1], i128[2]); show("_mm_maskstore_epi32", buffer, 64);
    fill();
    { __m256i __v = _mm256_maskload_epi64((long long *)buffer, i256[1]); show("_mm256_maskload_epi64", &__v, sizeof(__v)); }
    fill(); _mm256_maskstore_epi64((long long *)buffer, i256[1], i256[2]); show("_mm256_maskstore_epi64", buffer, 64);
    fill();
    { __m128i __v = _mm_maskload_epi64((long long *)buffer, i128[1]); show("_mm_maskload_epi64", &__v, sizeof(__v)); }
    fill(); _mm_maskstore_epi64((long long *)buffer, i128[1], i128[2]); show("_mm_maskstore_epi64", buffer, 64);
    fill();
    { __m256 __v = _mm256_maskload_ps((float *)buffer, i256[1]); show("_mm256_maskload_ps", &__v, sizeof(__v)); }
    fill(); _mm256_maskstore_ps((float *)buffer, i256[1], f256[2]); show("_mm256_maskstore_ps", buffer, 64);
    fill();
    { __m128 __v = _mm_maskload_ps((float *)buffer, i128[1]); show("_mm_maskload_ps", &__v, sizeof(__v)); }
    fill(); _mm_maskstore_ps((float *)buffer, i128[1], f128[2]); show("_mm_maskstore_ps", buffer, 64);
    fill();
    { __m256d __v = _mm256_maskload_pd((double *)buffer, i256[1]); show("_mm256_maskload_pd", &__v, sizeof(__v)); }
    fill(); _mm256_maskstore_pd((double *)buffer, i256[1], d256[2]); show("_mm256_maskstore_pd", buffer, 64);
    fill();
    { __m128d __v = _mm_maskload_pd((double *)buffer, i128[1]); show("_mm_maskload_pd", &__v, sizeof(__v)); }
    fill(); _mm_maskstore_pd((double *)buffer, i128[1], d128[2]); show("_mm_maskstore_pd", buffer, 64);
    { __m128i __v = _mm256_castsi256_si128(i256[0]); show("_mm256_castsi256_si128#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm256_castsi256_si128(i256[1]); show("_mm256_castsi256_si128#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_castsi128_si256(i128[0]); show("_mm256_castsi128_si256#0", &__v, 16); }
    { __m256i __v = _mm256_castsi128_si256(i128[1]); show("_mm256_castsi128_si256#1", &__v, 16); }
    { __m256i __v = _mm256_zextsi128_si256(i128[0]); show("_mm256_zextsi128_si256#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_zextsi128_si256(i128[1]); show("_mm256_zextsi128_si256#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm256_castps256_ps128(f256[0]); show("_mm256_castps256_ps128#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm256_castps256_ps128(f256[1]); show("_mm256_castps256_ps128#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_castps128_ps256(f128[0]); show("_mm256_castps128_ps256#0", &__v, 16); }
    { __m256 __v = _mm256_castps128_ps256(f128[1]); show("_mm256_castps128_ps256#1", &__v, 16); }
    { __m256 __v = _mm256_zextps128_ps256(f128[0]); show("_mm256_zextps128_ps256#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_zextps128_ps256(f128[1]); show("_mm256_zextps128_ps256#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm256_castpd256_pd128(d256[0]); show("_mm256_castpd256_pd128#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm256_castpd256_pd128(d256[1]); show("_mm256_castpd256_pd128#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_castpd128_pd256(d128[0]); show("_mm256_castpd128_pd256#0", &__v, 16); }
    { __m256d __v = _mm256_castpd128_pd256(d128[1]); show("_mm256_castpd128_pd256#1", &__v, 16); }
    { __m256d __v = _mm256_zextpd128_pd256(d128[0]); show("_mm256_zextpd128_pd256#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_zextpd128_pd256(d128[1]); show("_mm256_zextpd128_pd256#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_castps_si256(f256[0]); show("_mm256_castps_si256#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_castps_si256(f256[1]); show("_mm256_castps_si256#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_castsi256_ps(i256[0]); show("_mm256_castsi256_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_castsi256_ps(i256[1]); show("_mm256_castsi256_ps#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_castpd_si256(d256[0]); show("_mm256_castpd_si256#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_castpd_si256(d256[1]); show("_mm256_castpd_si256#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_castsi256_pd(i256[0]); show("_mm256_castsi256_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_castsi256_pd(i256[1]); show("_mm256_castsi256_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_castps_pd(f256[0]); show("_mm256_castps_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_castps_pd(f256[1]); show("_mm256_castps_pd#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_castpd_ps(d256[0]); show("_mm256_castpd_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_castpd_ps(d256[1]); show("_mm256_castpd_ps#1", &__v, sizeof(__v)); }
    printf("intrinsics: _mm256_cvtsi256_si32#0: %lld\n", (long long)(_mm256_cvtsi256_si32(i256[0])));
    printf("intrinsics: _mm256_cvtsi256_si32#1: %lld\n", (long long)(_mm256_cvtsi256_si32(i256[1])));
    { float __f = _mm256_cvtss_f32(f256[0]); show("_mm256_cvtss_f32#0", &__f, 4); }
    { float __f = _mm256_cvtss_f32(f256[1]); show("_mm256_cvtss_f32#1", &__f, 4); }
    { double __f = _mm256_cvtsd_f64(d256[0]); show("_mm256_cvtsd_f64#0", &__f, 8); }
    { double __f = _mm256_cvtsd_f64(d256[1]); show("_mm256_cvtsd_f64#1", &__f, 8); }
    { __m128i __v = _mm256_extracti128_si256(i256[0], 1); show("_mm256_extracti128_si256#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm256_extractf128_si256(i256[0], 1); show("_mm256_extractf128_si256#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm256_extractf128_ps(f256[0], 1); show("_mm256_extractf128_ps#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm256_extractf128_pd(d256[0], 1); show("_mm256_extractf128_pd#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_inserti128_si256(i256[0], i128[1], 1); show("_mm256_inserti128_si256#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_insertf128_si256(i256[0], i128[1], 1); show("_mm256_insertf128_si256#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_insertf128_ps(f256[0], f128[1], 1); show("_mm256_insertf128_ps#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_insertf128_pd(d256[0], d128[1], 1); show("_mm256_insertf128_pd#0", &__v, sizeof(__v)); }
    printf("intrinsics: _mm256_extract_epi8#0: %lld\n", (long long)(_mm256_extract_epi8(i256[0], 1)));
    printf("intrinsics: _mm256_extract_epi16#0: %lld\n", (long long)(_mm256_extract_epi16(i256[0], 1)));
    printf("intrinsics: _mm256_extract_epi32#0: %lld\n", (long long)(_mm256_extract_epi32(i256[0], 1)));
    printf("intrinsics: _mm256_extract_epi64#0: %lld\n", (long long)(_mm256_extract_epi64(i256[0], 1)));
    { __m256i __v = _mm256_insert_epi8(i256[0], -99978, 1); show("_mm256_insert_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_insert_epi16(i256[0], -99978, 1); show("_mm256_insert_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_insert_epi32(i256[0], -99978, 1); show("_mm256_insert_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_insert_epi64(i256[0], 27LL, 1); show("_mm256_insert_epi64#0", &__v, sizeof(__v)); }
    fill();
    { __m256 __v = _mm256_broadcast_ss((float *)buffer); show("_mm256_broadcast_ss", &__v, sizeof(__v)); }
    fill();
    { __m256d __v = _mm256_broadcast_sd((double *)buffer); show("_mm256_broadcast_sd", &__v, sizeof(__v)); }
    fill();
    { __m256 __v = _mm256_broadcast_ps((__m128 *)buffer); show("_mm256_broadcast_ps", &__v, sizeof(__v)); }
    fill();
    { __m256d __v = _mm256_broadcast_pd((__m128d *)buffer); show("_mm256_broadcast_pd", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_broadcastsi128_si256(i128[0]); show("_mm256_broadcastsi128_si256#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_broadcastsi128_si256(i128[1]); show("_mm256_broadcastsi128_si256#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm_broadcastsi128_si256(i128[0]); show("_mm_broadcastsi128_si256#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm_broadcastsi128_si256(i128[1]); show("_mm_broadcastsi128_si256#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_broadcastb_epi8(i128[0]); show("_mm256_broadcastb_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_broadcastb_epi8(i128[1]); show("_mm256_broadcastb_epi8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_broadcastw_epi16(i128[0]); show("_mm256_broadcastw_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_broadcastw_epi16(i128[1]); show("_mm256_broadcastw_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_broadcastd_epi32(i128[0]); show("_mm256_broadcastd_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_broadcastd_epi32(i128[1]); show("_mm256_broadcastd_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_broadcastq_epi64(i128[0]); show("_mm256_broadcastq_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_broadcastq_epi64(i128[1]); show("_mm256_broadcastq_epi64#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_broadcastss_ps(f128[0]); show("_mm256_broadcastss_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_broadcastss_ps(f128[1]); show("_mm256_broadcastss_ps#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_broadcastsd_pd(d128[0]); show("_mm256_broadcastsd_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_broadcastsd_pd(d128[1]); show("_mm256_broadcastsd_pd#1", &__v, sizeof(__v)); }
    _mm256_zeroupper();
    _mm256_zeroall();
    { __m256i __v = _mm256_add_epi8(i256[0], i256[1]); show("_mm256_add_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_add_epi8(i256[1], i256[2]); show("_mm256_add_epi8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_add_epi16(i256[0], i256[1]); show("_mm256_add_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_add_epi16(i256[1], i256[2]); show("_mm256_add_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_add_epi32(i256[0], i256[1]); show("_mm256_add_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_add_epi32(i256[1], i256[2]); show("_mm256_add_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_add_epi64(i256[0], i256[1]); show("_mm256_add_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_add_epi64(i256[1], i256[2]); show("_mm256_add_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sub_epi8(i256[0], i256[1]); show("_mm256_sub_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sub_epi8(i256[1], i256[2]); show("_mm256_sub_epi8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sub_epi16(i256[0], i256[1]); show("_mm256_sub_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sub_epi16(i256[1], i256[2]); show("_mm256_sub_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sub_epi32(i256[0], i256[1]); show("_mm256_sub_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sub_epi32(i256[1], i256[2]); show("_mm256_sub_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sub_epi64(i256[0], i256[1]); show("_mm256_sub_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sub_epi64(i256[1], i256[2]); show("_mm256_sub_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_adds_epi8(i256[0], i256[1]); show("_mm256_adds_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_adds_epi8(i256[1], i256[2]); show("_mm256_adds_epi8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_adds_epi16(i256[0], i256[1]); show("_mm256_adds_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_adds_epi16(i256[1], i256[2]); show("_mm256_adds_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_adds_epu8(i256[0], i256[1]); show("_mm256_adds_epu8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_adds_epu8(i256[1], i256[2]); show("_mm256_adds_epu8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_adds_epu16(i256[0], i256[1]); show("_mm256_adds_epu16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_adds_epu16(i256[1], i256[2]); show("_mm256_adds_epu16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_subs_epi8(i256[0], i256[1]); show("_mm256_subs_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_subs_epi8(i256[1], i256[2]); show("_mm256_subs_epi8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_subs_epi16(i256[0], i256[1]); show("_mm256_subs_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_subs_epi16(i256[1], i256[2]); show("_mm256_subs_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_subs_epu8(i256[0], i256[1]); show("_mm256_subs_epu8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_subs_epu8(i256[1], i256[2]); show("_mm256_subs_epu8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_subs_epu16(i256[0], i256[1]); show("_mm256_subs_epu16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_subs_epu16(i256[1], i256[2]); show("_mm256_subs_epu16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mullo_epi16(i256[0], i256[1]); show("_mm256_mullo_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mullo_epi16(i256[1], i256[2]); show("_mm256_mullo_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mullo_epi32(i256[0], i256[1]); show("_mm256_mullo_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mullo_epi32(i256[1], i256[2]); show("_mm256_mullo_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mulhi_epi16(i256[0], i256[1]); show("_mm256_mulhi_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mulhi_epi16(i256[1], i256[2]); show("_mm256_mulhi_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mulhi_epu16(i256[0], i256[1]); show("_mm256_mulhi_epu16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mulhi_epu16(i256[1], i256[2]); show("_mm256_mulhi_epu16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mul_epi32(i256[0], i256[1]); show("_mm256_mul_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mul_epi32(i256[1], i256[2]); show("_mm256_mul_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mul_epu32(i256[0], i256[1]); show("_mm256_mul_epu32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mul_epu32(i256[1], i256[2]); show("_mm256_mul_epu32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_madd_epi16(i256[0], i256[1]); show("_mm256_madd_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_madd_epi16(i256[1], i256[2]); show("_mm256_madd_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_maddubs_epi16(i256[0], i256[1]); show("_mm256_maddubs_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_maddubs_epi16(i256[1], i256[2]); show("_mm256_maddubs_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sad_epu8(i256[0], i256[1]); show("_mm256_sad_epu8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sad_epu8(i256[1], i256[2]); show("_mm256_sad_epu8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_avg_epu8(i256[0], i256[1]); show("_mm256_avg_epu8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_avg_epu8(i256[1], i256[2]); show("_mm256_avg_epu8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_avg_epu16(i256[0], i256[1]); show("_mm256_avg_epu16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_avg_epu16(i256[1], i256[2]); show("_mm256_avg_epu16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mulhrs_epi16(i256[0], i256[1]); show("_mm256_mulhrs_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mulhrs_epi16(i256[1], i256[2]); show("_mm256_mulhrs_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_min_epi8(i256[0], i256[1]); show("_mm256_min_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_min_epi8(i256[1], i256[2]); show("_mm256_min_epi8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_min_epi16(i256[0], i256[1]); show("_mm256_min_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_min_epi16(i256[1], i256[2]); show("_mm256_min_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_min_epi32(i256[0], i256[1]); show("_mm256_min_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_min_epi32(i256[1], i256[2]); show("_mm256_min_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_min_epu8(i256[0], i256[1]); show("_mm256_min_epu8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_min_epu8(i256[1], i256[2]); show("_mm256_min_epu8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_min_epu16(i256[0], i256[1]); show("_mm256_min_epu16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_min_epu16(i256[1], i256[2]); show("_mm256_min_epu16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_min_epu32(i256[0], i256[1]); show("_mm256_min_epu32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_min_epu32(i256[1], i256[2]); show("_mm256_min_epu32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_max_epi8(i256[0], i256[1]); show("_mm256_max_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_max_epi8(i256[1], i256[2]); show("_mm256_max_epi8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_max_epi16(i256[0], i256[1]); show("_mm256_max_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_max_epi16(i256[1], i256[2]); show("_mm256_max_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_max_epi32(i256[0], i256[1]); show("_mm256_max_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_max_epi32(i256[1], i256[2]); show("_mm256_max_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_max_epu8(i256[0], i256[1]); show("_mm256_max_epu8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_max_epu8(i256[1], i256[2]); show("_mm256_max_epu8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_max_epu16(i256[0], i256[1]); show("_mm256_max_epu16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_max_epu16(i256[1], i256[2]); show("_mm256_max_epu16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_max_epu32(i256[0], i256[1]); show("_mm256_max_epu32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_max_epu32(i256[1], i256[2]); show("_mm256_max_epu32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sign_epi8(i256[0], i256[1]); show("_mm256_sign_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sign_epi8(i256[1], i256[2]); show("_mm256_sign_epi8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sign_epi16(i256[0], i256[1]); show("_mm256_sign_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sign_epi16(i256[1], i256[2]); show("_mm256_sign_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sign_epi32(i256[0], i256[1]); show("_mm256_sign_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sign_epi32(i256[1], i256[2]); show("_mm256_sign_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_hadd_epi16(i256[0], i256[1]); show("_mm256_hadd_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_hadd_epi16(i256[1], i256[2]); show("_mm256_hadd_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_hadd_epi32(i256[0], i256[1]); show("_mm256_hadd_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_hadd_epi32(i256[1], i256[2]); show("_mm256_hadd_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_hadds_epi16(i256[0], i256[1]); show("_mm256_hadds_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_hadds_epi16(i256[1], i256[2]); show("_mm256_hadds_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_hsub_epi16(i256[0], i256[1]); show("_mm256_hsub_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_hsub_epi16(i256[1], i256[2]); show("_mm256_hsub_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_hsub_epi32(i256[0], i256[1]); show("_mm256_hsub_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_hsub_epi32(i256[1], i256[2]); show("_mm256_hsub_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_hsubs_epi16(i256[0], i256[1]); show("_mm256_hsubs_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_hsubs_epi16(i256[1], i256[2]); show("_mm256_hsubs_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_and_si256(i256[0], i256[1]); show("_mm256_and_si256#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_and_si256(i256[1], i256[2]); show("_mm256_and_si256#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_or_si256(i256[0], i256[1]); show("_mm256_or_si256#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_or_si256(i256[1], i256[2]); show("_mm256_or_si256#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_xor_si256(i256[0], i256[1]); show("_mm256_xor_si256#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_xor_si256(i256[1], i256[2]); show("_mm256_xor_si256#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_andnot_si256(i256[0], i256[1]); show("_mm256_andnot_si256#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_andnot_si256(i256[1], i256[2]); show("_mm256_andnot_si256#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpeq_epi8(i256[0], i256[1]); show("_mm256_cmpeq_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpeq_epi8(i256[1], i256[2]); show("_mm256_cmpeq_epi8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpeq_epi16(i256[0], i256[1]); show("_mm256_cmpeq_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpeq_epi16(i256[1], i256[2]); show("_mm256_cmpeq_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpeq_epi32(i256[0], i256[1]); show("_mm256_cmpeq_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpeq_epi32(i256[1], i256[2]); show("_mm256_cmpeq_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpeq_epi64(i256[0], i256[1]); show("_mm256_cmpeq_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpeq_epi64(i256[1], i256[2]); show("_mm256_cmpeq_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpgt_epi8(i256[0], i256[1]); show("_mm256_cmpgt_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpgt_epi8(i256[1], i256[2]); show("_mm256_cmpgt_epi8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpgt_epi16(i256[0], i256[1]); show("_mm256_cmpgt_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpgt_epi16(i256[1], i256[2]); show("_mm256_cmpgt_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpgt_epi32(i256[0], i256[1]); show("_mm256_cmpgt_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpgt_epi32(i256[1], i256[2]); show("_mm256_cmpgt_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpgt_epi64(i256[0], i256[1]); show("_mm256_cmpgt_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cmpgt_epi64(i256[1], i256[2]); show("_mm256_cmpgt_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_shuffle_epi8(i256[0], i256[1]); show("_mm256_shuffle_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_shuffle_epi8(i256[1], i256[2]); show("_mm256_shuffle_epi8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpacklo_epi8(i256[0], i256[1]); show("_mm256_unpacklo_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpacklo_epi8(i256[1], i256[2]); show("_mm256_unpacklo_epi8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpacklo_epi16(i256[0], i256[1]); show("_mm256_unpacklo_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpacklo_epi16(i256[1], i256[2]); show("_mm256_unpacklo_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpacklo_epi32(i256[0], i256[1]); show("_mm256_unpacklo_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpacklo_epi32(i256[1], i256[2]); show("_mm256_unpacklo_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpacklo_epi64(i256[0], i256[1]); show("_mm256_unpacklo_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpacklo_epi64(i256[1], i256[2]); show("_mm256_unpacklo_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpackhi_epi8(i256[0], i256[1]); show("_mm256_unpackhi_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpackhi_epi8(i256[1], i256[2]); show("_mm256_unpackhi_epi8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpackhi_epi16(i256[0], i256[1]); show("_mm256_unpackhi_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpackhi_epi16(i256[1], i256[2]); show("_mm256_unpackhi_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpackhi_epi32(i256[0], i256[1]); show("_mm256_unpackhi_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpackhi_epi32(i256[1], i256[2]); show("_mm256_unpackhi_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpackhi_epi64(i256[0], i256[1]); show("_mm256_unpackhi_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_unpackhi_epi64(i256[1], i256[2]); show("_mm256_unpackhi_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_packs_epi16(i256[0], i256[1]); show("_mm256_packs_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_packs_epi16(i256[1], i256[2]); show("_mm256_packs_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_packs_epi32(i256[0], i256[1]); show("_mm256_packs_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_packs_epi32(i256[1], i256[2]); show("_mm256_packs_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_packus_epi16(i256[0], i256[1]); show("_mm256_packus_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_packus_epi16(i256[1], i256[2]); show("_mm256_packus_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_packus_epi32(i256[0], i256[1]); show("_mm256_packus_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_packus_epi32(i256[1], i256[2]); show("_mm256_packus_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sllv_epi32(i256[0], i256[1]); show("_mm256_sllv_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sllv_epi32(i256[1], i256[2]); show("_mm256_sllv_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sllv_epi64(i256[0], i256[1]); show("_mm256_sllv_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sllv_epi64(i256[1], i256[2]); show("_mm256_sllv_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srlv_epi32(i256[0], i256[1]); show("_mm256_srlv_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srlv_epi32(i256[1], i256[2]); show("_mm256_srlv_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srlv_epi64(i256[0], i256[1]); show("_mm256_srlv_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srlv_epi64(i256[1], i256[2]); show("_mm256_srlv_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srav_epi32(i256[0], i256[1]); show("_mm256_srav_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srav_epi32(i256[1], i256[2]); show("_mm256_srav_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_permutevar8x32_epi32(i256[0], i256[1]); show("_mm256_permutevar8x32_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_permutevar8x32_epi32(i256[1], i256[2]); show("_mm256_permutevar8x32_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sllv_epi32(i128[0], i128[1]); show("_mm_sllv_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sllv_epi32(i128[1], i128[2]); show("_mm_sllv_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sllv_epi64(i128[0], i128[1]); show("_mm_sllv_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sllv_epi64(i128[1], i128[2]); show("_mm_sllv_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srlv_epi32(i128[0], i128[1]); show("_mm_srlv_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srlv_epi32(i128[1], i128[2]); show("_mm_srlv_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srlv_epi64(i128[0], i128[1]); show("_mm_srlv_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srlv_epi64(i128[1], i128[2]); show("_mm_srlv_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srav_epi32(i128[0], i128[1]); show("_mm_srav_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srav_epi32(i128[1], i128[2]); show("_mm_srav_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_slli_epi16(i256[0], 3); show("_mm256_slli_epi16#0/3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_slli_epi16(i256[0], 31); show("_mm256_slli_epi16#0/31", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_slli_epi16(i256[0], 33); show("_mm256_slli_epi16#0/33", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_slli_epi32(i256[0], 3); show("_mm256_slli_epi32#0/3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_slli_epi32(i256[0], 31); show("_mm256_slli_epi32#0/31", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_slli_epi32(i256[0], 33); show("_mm256_slli_epi32#0/33", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_slli_epi64(i256[0], 3); show("_mm256_slli_epi64#0/3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_slli_epi64(i256[0], 31); show("_mm256_slli_epi64#0/31", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_slli_epi64(i256[0], 33); show("_mm256_slli_epi64#0/33", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srli_epi16(i256[0], 3); show("_mm256_srli_epi16#0/3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srli_epi16(i256[0], 31); show("_mm256_srli_epi16#0/31", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srli_epi16(i256[0], 33); show("_mm256_srli_epi16#0/33", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srli_epi32(i256[0], 3); show("_mm256_srli_epi32#0/3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srli_epi32(i256[0], 31); show("_mm256_srli_epi32#0/31", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srli_epi32(i256[0], 33); show("_mm256_srli_epi32#0/33", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srli_epi64(i256[0], 3); show("_mm256_srli_epi64#0/3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srli_epi64(i256[0], 31); show("_mm256_srli_epi64#0/31", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srli_epi64(i256[0], 33); show("_mm256_srli_epi64#0/33", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srai_epi16(i256[0], 3); show("_mm256_srai_epi16#0/3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srai_epi16(i256[0], 31); show("_mm256_srai_epi16#0/31", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srai_epi16(i256[0], 33); show("_mm256_srai_epi16#0/33", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srai_epi32(i256[0], 3); show("_mm256_srai_epi32#0/3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srai_epi32(i256[0], 31); show("_mm256_srai_epi32#0/31", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srai_epi32(i256[0], 33); show("_mm256_srai_epi32#0/33", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sll_epi16(i256[0], i128[1]); show("_mm256_sll_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sll_epi16(i256[1], i128[2]); show("_mm256_sll_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sll_epi32(i256[0], i128[1]); show("_mm256_sll_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sll_epi32(i256[1], i128[2]); show("_mm256_sll_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sll_epi64(i256[0], i128[1]); show("_mm256_sll_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sll_epi64(i256[1], i128[2]); show("_mm256_sll_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srl_epi16(i256[0], i128[1]); show("_mm256_srl_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srl_epi16(i256[1], i128[2]); show("_mm256_srl_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srl_epi32(i256[0], i128[1]); show("_mm256_srl_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srl_epi32(i256[1], i128[2]); show("_mm256_srl_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srl_epi64(i256[0], i128[1]); show("_mm256_srl_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srl_epi64(i256[1], i128[2]); show("_mm256_srl_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sra_epi16(i256[0], i128[1]); show("_mm256_sra_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sra_epi16(i256[1], i128[2]); show("_mm256_sra_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sra_epi32(i256[0], i128[1]); show("_mm256_sra_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_sra_epi32(i256[1], i128[2]); show("_mm256_sra_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_abs_epi8(i256[0]); show("_mm256_abs_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_abs_epi8(i256[1]); show("_mm256_abs_epi8#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_abs_epi16(i256[0]); show("_mm256_abs_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_abs_epi16(i256[1]); show("_mm256_abs_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_abs_epi32(i256[0]); show("_mm256_abs_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_abs_epi32(i256[1]); show("_mm256_abs_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepi8_epi16(i128[0]); show("_mm256_cvtepi8_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepi8_epi16(i128[1]); show("_mm256_cvtepi8_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepi8_epi32(i128[0]); show("_mm256_cvtepi8_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepi8_epi32(i128[1]); show("_mm256_cvtepi8_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepi8_epi64(i128[0]); show("_mm256_cvtepi8_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepi8_epi64(i128[1]); show("_mm256_cvtepi8_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepi16_epi32(i128[0]); show("_mm256_cvtepi16_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepi16_epi32(i128[1]); show("_mm256_cvtepi16_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepi16_epi64(i128[0]); show("_mm256_cvtepi16_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepi16_epi64(i128[1]); show("_mm256_cvtepi16_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepi32_epi64(i128[0]); show("_mm256_cvtepi32_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepi32_epi64(i128[1]); show("_mm256_cvtepi32_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepu8_epi16(i128[0]); show("_mm256_cvtepu8_epi16#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepu8_epi16(i128[1]); show("_mm256_cvtepu8_epi16#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepu8_epi32(i128[0]); show("_mm256_cvtepu8_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepu8_epi32(i128[1]); show("_mm256_cvtepu8_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepu8_epi64(i128[0]); show("_mm256_cvtepu8_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepu8_epi64(i128[1]); show("_mm256_cvtepu8_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepu16_epi32(i128[0]); show("_mm256_cvtepu16_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepu16_epi32(i128[1]); show("_mm256_cvtepu16_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepu16_epi64(i128[0]); show("_mm256_cvtepu16_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepu16_epi64(i128[1]); show("_mm256_cvtepu16_epi64#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepu32_epi64(i128[0]); show("_mm256_cvtepu32_epi64#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtepu32_epi64(i128[1]); show("_mm256_cvtepu32_epi64#1", &__v, sizeof(__v)); }
    printf("intrinsics: _mm256_movemask_epi8#0: %lld\n", (long long)(_mm256_movemask_epi8(i256[0])));
    printf("intrinsics: _mm256_movemask_epi8#1: %lld\n", (long long)(_mm256_movemask_epi8(i256[1])));
    printf("intrinsics: _mm256_movemask_ps#0: %lld\n", (long long)(_mm256_movemask_ps(f256[0])));
    printf("intrinsics: _mm256_movemask_ps#1: %lld\n", (long long)(_mm256_movemask_ps(f256[1])));
    printf("intrinsics: _mm256_movemask_pd#0: %lld\n", (long long)(_mm256_movemask_pd(d256[0])));
    printf("intrinsics: _mm256_movemask_pd#1: %lld\n", (long long)(_mm256_movemask_pd(d256[1])));
    printf("intrinsics: _mm256_testz_si256#0: %lld\n", (long long)(_mm256_testz_si256(i256[0], i256[1])));
    printf("intrinsics: _mm256_testz_si256#1: %lld\n", (long long)(_mm256_testz_si256(i256[1], i256[2])));
    printf("intrinsics: _mm256_testc_si256#0: %lld\n", (long long)(_mm256_testc_si256(i256[0], i256[1])));
    printf("intrinsics: _mm256_testc_si256#1: %lld\n", (long long)(_mm256_testc_si256(i256[1], i256[2])));
    printf("intrinsics: _mm256_testnzc_si256#0: %lld\n", (long long)(_mm256_testnzc_si256(i256[0], i256[1])));
    printf("intrinsics: _mm256_testnzc_si256#1: %lld\n", (long long)(_mm256_testnzc_si256(i256[1], i256[2])));
    { __m256i __v = _mm256_blendv_epi8(i256[0], i256[1], i256[2]); show("_mm256_blendv_epi8#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_blendv_epi8(i256[1], i256[2], i256[0]); show("_mm256_blendv_epi8#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_add_ps(f256[0], f256[1]); show("_mm256_add_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_add_ps(f256[1], f256[2]); show("_mm256_add_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_sub_ps(f256[0], f256[1]); show("_mm256_sub_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_sub_ps(f256[1], f256[2]); show("_mm256_sub_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_mul_ps(f256[0], f256[1]); show("_mm256_mul_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_mul_ps(f256[1], f256[2]); show("_mm256_mul_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_div_ps(f256[0], f256[1]); show("_mm256_div_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_div_ps(f256[1], f256[2]); show("_mm256_div_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_min_ps(f256[0], f256[1]); show("_mm256_min_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_min_ps(f256[1], f256[2]); show("_mm256_min_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_max_ps(f256[0], f256[1]); show("_mm256_max_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_max_ps(f256[1], f256[2]); show("_mm256_max_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_and_ps(f256[0], f256[1]); show("_mm256_and_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_and_ps(f256[1], f256[2]); show("_mm256_and_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_or_ps(f256[0], f256[1]); show("_mm256_or_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_or_ps(f256[1], f256[2]); show("_mm256_or_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_xor_ps(f256[0], f256[1]); show("_mm256_xor_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_xor_ps(f256[1], f256[2]); show("_mm256_xor_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_andnot_ps(f256[0], f256[1]); show("_mm256_andnot_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_andnot_ps(f256[1], f256[2]); show("_mm256_andnot_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_unpacklo_ps(f256[0], f256[1]); show("_mm256_unpacklo_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_unpacklo_ps(f256[1], f256[2]); show("_mm256_unpacklo_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_unpackhi_ps(f256[0], f256[1]); show("_mm256_unpackhi_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_unpackhi_ps(f256[1], f256[2]); show("_mm256_unpackhi_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_hadd_ps(f256[0], f256[1]); show("_mm256_hadd_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_hadd_ps(f256[1], f256[2]); show("_mm256_hadd_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_hsub_ps(f256[0], f256[1]); show("_mm256_hsub_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_hsub_ps(f256[1], f256[2]); show("_mm256_hsub_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_addsub_ps(f256[0], f256[1]); show("_mm256_addsub_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_addsub_ps(f256[1], f256[2]); show("_mm256_addsub_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_sqrt_ps(f256[0]); show("_mm256_sqrt_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_sqrt_ps(f256[1]); show("_mm256_sqrt_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_permutevar_ps(f256[0], f256[1]); show("_mm256_permutevar_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_permutevar_ps(f256[1], f256[2]); show("_mm256_permutevar_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_permutevar_ps(f128[0], f128[1]); show("_mm_permutevar_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_permutevar_ps(f128[1], f128[2]); show("_mm_permutevar_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_blendv_ps(f256[0], f256[1], f256[2]); show("_mm256_blendv_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_blendv_ps(f256[1], f256[2], f256[0]); show("_mm256_blendv_ps#1", &__v, sizeof(__v)); }
    printf("intrinsics: _mm256_testz_ps#0: %lld\n", (long long)(_mm256_testz_ps(f256[0], f256[1])));
    printf("intrinsics: _mm256_testz_ps#1: %lld\n", (long long)(_mm256_testz_ps(f256[1], f256[2])));
    printf("intrinsics: _mm256_testc_ps#0: %lld\n", (long long)(_mm256_testc_ps(f256[0], f256[1])));
    printf("intrinsics: _mm256_testc_ps#1: %lld\n", (long long)(_mm256_testc_ps(f256[1], f256[2])));
    printf("intrinsics: _mm256_testnzc_ps#0: %lld\n", (long long)(_mm256_testnzc_ps(f256[0], f256[1])));
    printf("intrinsics: _mm256_testnzc_ps#1: %lld\n", (long long)(_mm256_testnzc_ps(f256[1], f256[2])));
    { __m256 __v = _mm256_fmadd_ps(f256[0], f256[1], f256[2]); show("_mm256_fmadd_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_fmadd_ps(f256[1], f256[2], f256[0]); show("_mm256_fmadd_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fmadd_ps(f128[0], f128[1], f128[2]); show("_mm_fmadd_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fmadd_ps(f128[1], f128[2], f128[0]); show("_mm_fmadd_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_fmsub_ps(f256[0], f256[1], f256[2]); show("_mm256_fmsub_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_fmsub_ps(f256[1], f256[2], f256[0]); show("_mm256_fmsub_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fmsub_ps(f128[0], f128[1], f128[2]); show("_mm_fmsub_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fmsub_ps(f128[1], f128[2], f128[0]); show("_mm_fmsub_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_fnmadd_ps(f256[0], f256[1], f256[2]); show("_mm256_fnmadd_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_fnmadd_ps(f256[1], f256[2], f256[0]); show("_mm256_fnmadd_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fnmadd_ps(f128[0], f128[1], f128[2]); show("_mm_fnmadd_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fnmadd_ps(f128[1], f128[2], f128[0]); show("_mm_fnmadd_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_fnmsub_ps(f256[0], f256[1], f256[2]); show("_mm256_fnmsub_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_fnmsub_ps(f256[1], f256[2], f256[0]); show("_mm256_fnmsub_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fnmsub_ps(f128[0], f128[1], f128[2]); show("_mm_fnmsub_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fnmsub_ps(f128[1], f128[2], f128[0]); show("_mm_fnmsub_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_fmaddsub_ps(f256[0], f256[1], f256[2]); show("_mm256_fmaddsub_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_fmaddsub_ps(f256[1], f256[2], f256[0]); show("_mm256_fmaddsub_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fmaddsub_ps(f128[0], f128[1], f128[2]); show("_mm_fmaddsub_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fmaddsub_ps(f128[1], f128[2], f128[0]); show("_mm_fmaddsub_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_fmsubadd_ps(f256[0], f256[1], f256[2]); show("_mm256_fmsubadd_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_fmsubadd_ps(f256[1], f256[2], f256[0]); show("_mm256_fmsubadd_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fmsubadd_ps(f128[0], f128[1], f128[2]); show("_mm_fmsubadd_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fmsubadd_ps(f128[1], f128[2], f128[0]); show("_mm_fmsubadd_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fmadd_ss(f128[0], f128[1], f128[2]); show("_mm_fmadd_ss#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fmadd_ss(f128[1], f128[2], f128[0]); show("_mm_fmadd_ss#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fmsub_ss(f128[0], f128[1], f128[2]); show("_mm_fmsub_ss#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fmsub_ss(f128[1], f128[2], f128[0]); show("_mm_fmsub_ss#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fnmadd_ss(f128[0], f128[1], f128[2]); show("_mm_fnmadd_ss#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fnmadd_ss(f128[1], f128[2], f128[0]); show("_mm_fnmadd_ss#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fnmsub_ss(f128[0], f128[1], f128[2]); show("_mm_fnmsub_ss#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_fnmsub_ss(f128[1], f128[2], f128[0]); show("_mm_fnmsub_ss#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_add_pd(d256[0], d256[1]); show("_mm256_add_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_add_pd(d256[1], d256[2]); show("_mm256_add_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_sub_pd(d256[0], d256[1]); show("_mm256_sub_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_sub_pd(d256[1], d256[2]); show("_mm256_sub_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_mul_pd(d256[0], d256[1]); show("_mm256_mul_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_mul_pd(d256[1], d256[2]); show("_mm256_mul_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_div_pd(d256[0], d256[1]); show("_mm256_div_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_div_pd(d256[1], d256[2]); show("_mm256_div_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_min_pd(d256[0], d256[1]); show("_mm256_min_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_min_pd(d256[1], d256[2]); show("_mm256_min_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_max_pd(d256[0], d256[1]); show("_mm256_max_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_max_pd(d256[1], d256[2]); show("_mm256_max_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_and_pd(d256[0], d256[1]); show("_mm256_and_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_and_pd(d256[1], d256[2]); show("_mm256_and_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_or_pd(d256[0], d256[1]); show("_mm256_or_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_or_pd(d256[1], d256[2]); show("_mm256_or_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_xor_pd(d256[0], d256[1]); show("_mm256_xor_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_xor_pd(d256[1], d256[2]); show("_mm256_xor_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_andnot_pd(d256[0], d256[1]); show("_mm256_andnot_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_andnot_pd(d256[1], d256[2]); show("_mm256_andnot_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_unpacklo_pd(d256[0], d256[1]); show("_mm256_unpacklo_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_unpacklo_pd(d256[1], d256[2]); show("_mm256_unpacklo_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_unpackhi_pd(d256[0], d256[1]); show("_mm256_unpackhi_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_unpackhi_pd(d256[1], d256[2]); show("_mm256_unpackhi_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_hadd_pd(d256[0], d256[1]); show("_mm256_hadd_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_hadd_pd(d256[1], d256[2]); show("_mm256_hadd_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_hsub_pd(d256[0], d256[1]); show("_mm256_hsub_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_hsub_pd(d256[1], d256[2]); show("_mm256_hsub_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_addsub_pd(d256[0], d256[1]); show("_mm256_addsub_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_addsub_pd(d256[1], d256[2]); show("_mm256_addsub_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_sqrt_pd(d256[0]); show("_mm256_sqrt_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_sqrt_pd(d256[1]); show("_mm256_sqrt_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_permutevar_pd(d256[0], d256[1]); show("_mm256_permutevar_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_permutevar_pd(d256[1], d256[2]); show("_mm256_permutevar_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_permutevar_pd(d128[0], d128[1]); show("_mm_permutevar_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_permutevar_pd(d128[1], d128[2]); show("_mm_permutevar_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_blendv_pd(d256[0], d256[1], d256[2]); show("_mm256_blendv_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_blendv_pd(d256[1], d256[2], d256[0]); show("_mm256_blendv_pd#1", &__v, sizeof(__v)); }
    printf("intrinsics: _mm256_testz_pd#0: %lld\n", (long long)(_mm256_testz_pd(d256[0], d256[1])));
    printf("intrinsics: _mm256_testz_pd#1: %lld\n", (long long)(_mm256_testz_pd(d256[1], d256[2])));
    printf("intrinsics: _mm256_testc_pd#0: %lld\n", (long long)(_mm256_testc_pd(d256[0], d256[1])));
    printf("intrinsics: _mm256_testc_pd#1: %lld\n", (long long)(_mm256_testc_pd(d256[1], d256[2])));
    printf("intrinsics: _mm256_testnzc_pd#0: %lld\n", (long long)(_mm256_testnzc_pd(d256[0], d256[1])));
    printf("intrinsics: _mm256_testnzc_pd#1: %lld\n", (long long)(_mm256_testnzc_pd(d256[1], d256[2])));
    { __m256d __v = _mm256_fmadd_pd(d256[0], d256[1], d256[2]); show("_mm256_fmadd_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_fmadd_pd(d256[1], d256[2], d256[0]); show("_mm256_fmadd_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fmadd_pd(d128[0], d128[1], d128[2]); show("_mm_fmadd_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fmadd_pd(d128[1], d128[2], d128[0]); show("_mm_fmadd_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_fmsub_pd(d256[0], d256[1], d256[2]); show("_mm256_fmsub_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_fmsub_pd(d256[1], d256[2], d256[0]); show("_mm256_fmsub_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fmsub_pd(d128[0], d128[1], d128[2]); show("_mm_fmsub_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fmsub_pd(d128[1], d128[2], d128[0]); show("_mm_fmsub_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_fnmadd_pd(d256[0], d256[1], d256[2]); show("_mm256_fnmadd_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_fnmadd_pd(d256[1], d256[2], d256[0]); show("_mm256_fnmadd_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fnmadd_pd(d128[0], d128[1], d128[2]); show("_mm_fnmadd_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fnmadd_pd(d128[1], d128[2], d128[0]); show("_mm_fnmadd_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_fnmsub_pd(d256[0], d256[1], d256[2]); show("_mm256_fnmsub_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_fnmsub_pd(d256[1], d256[2], d256[0]); show("_mm256_fnmsub_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fnmsub_pd(d128[0], d128[1], d128[2]); show("_mm_fnmsub_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fnmsub_pd(d128[1], d128[2], d128[0]); show("_mm_fnmsub_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_fmaddsub_pd(d256[0], d256[1], d256[2]); show("_mm256_fmaddsub_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_fmaddsub_pd(d256[1], d256[2], d256[0]); show("_mm256_fmaddsub_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fmaddsub_pd(d128[0], d128[1], d128[2]); show("_mm_fmaddsub_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fmaddsub_pd(d128[1], d128[2], d128[0]); show("_mm_fmaddsub_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_fmsubadd_pd(d256[0], d256[1], d256[2]); show("_mm256_fmsubadd_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_fmsubadd_pd(d256[1], d256[2], d256[0]); show("_mm256_fmsubadd_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fmsubadd_pd(d128[0], d128[1], d128[2]); show("_mm_fmsubadd_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fmsubadd_pd(d128[1], d128[2], d128[0]); show("_mm_fmsubadd_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fmadd_sd(d128[0], d128[1], d128[2]); show("_mm_fmadd_sd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fmadd_sd(d128[1], d128[2], d128[0]); show("_mm_fmadd_sd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fmsub_sd(d128[0], d128[1], d128[2]); show("_mm_fmsub_sd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fmsub_sd(d128[1], d128[2], d128[0]); show("_mm_fmsub_sd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fnmadd_sd(d128[0], d128[1], d128[2]); show("_mm_fnmadd_sd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fnmadd_sd(d128[1], d128[2], d128[0]); show("_mm_fnmadd_sd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fnmsub_sd(d128[0], d128[1], d128[2]); show("_mm_fnmsub_sd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_fnmsub_sd(d128[1], d128[2], d128[0]); show("_mm_fnmsub_sd#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_permutevar8x32_ps(f256[0], f256[1]); show("_mm256_permutevar8x32_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_permutevar8x32_ps(f256[1], f256[2]); show("_mm256_permutevar8x32_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_rcp_ps(f256[0]); show("_mm256_rcp_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_rcp_ps(f256[1]); show("_mm256_rcp_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_rsqrt_ps(f256[0]); show("_mm256_rsqrt_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_rsqrt_ps(f256[1]); show("_mm256_rsqrt_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_movehdup_ps(f256[0]); show("_mm256_movehdup_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_movehdup_ps(f256[1]); show("_mm256_movehdup_ps#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_moveldup_ps(f256[0]); show("_mm256_moveldup_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_moveldup_ps(f256[1]); show("_mm256_moveldup_ps#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_movedup_pd(d256[0]); show("_mm256_movedup_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_movedup_pd(d256[1]); show("_mm256_movedup_pd#1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_cvtepi32_ps(i256[0]); show("_mm256_cvtepi32_ps#0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_cvtepi32_ps(i256[1]); show("_mm256_cvtepi32_ps#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtps_epi32(f256[0]); show("_mm256_cvtps_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvtps_epi32(f256[1]); show("_mm256_cvtps_epi32#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvttps_epi32(f256[0]); show("_mm256_cvttps_epi32#0", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_cvttps_epi32(f256[1]); show("_mm256_cvttps_epi32#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_cvtepi32_pd(i128[0]); show("_mm256_cvtepi32_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_cvtepi32_pd(i128[1]); show("_mm256_cvtepi32_pd#1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_cvtps_pd(f128[0]); show("_mm256_cvtps_pd#0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_cvtps_pd(f128[1]); show("_mm256_cvtps_pd#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm256_cvtpd_ps(d256[0]); show("_mm256_cvtpd_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm256_cvtpd_ps(d256[1]); show("_mm256_cvtpd_ps#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm256_cvtpd_epi32(d256[0]); show("_mm256_cvtpd_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm256_cvtpd_epi32(d256[1]); show("_mm256_cvtpd_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm256_cvttpd_epi32(d256[0]); show("_mm256_cvttpd_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm256_cvttpd_epi32(d256[1]); show("_mm256_cvttpd_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_setzero_si128(); show("_mm_setzero_si128#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_setzero_ps(); show("_mm_setzero_ps#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_setzero_pd(); show("_mm_setzero_pd#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_set1_epi8((char)-91); show("_mm_set1_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_set_epi8((char)-91, (char)-43, (char)5, (char)53, (char)101, (char)-107, (char)-59, (char)-11, (char)37, (char)85, (char)-123, (char)-75, (char)-27, (char)21, (char)69, (char)117); show("_mm_set_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_setr_epi8((char)-91, (char)-43, (char)5, (char)53, (char)101, (char)-107, (char)-59, (char)-11, (char)37, (char)85, (char)-123, (char)-75, (char)-27, (char)21, (char)69, (char)117); show("_mm_setr_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_set1_epi16((short)-28669); show("_mm_set1_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_set_epi16((short)-28669, (short)-24563, (short)-20457, (short)-16351, (short)-12245, (short)-8139, (short)-4033, (short)73); show("_mm_set_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_setr_epi16((short)-28669, (short)-24563, (short)-20457, (short)-16351, (short)-12245, (short)-8139, (short)-4033, (short)73); show("_mm_setr_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_set1_epi32(3); show("_mm_set1_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_set_epi32(3, -99978, 42, -99939); show("_mm_set_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_setr_epi32(3, -99978, 42, -99939); show("_mm_setr_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_set1_epi64x(-999999993LL); show("_mm_set1_epi64x#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_set_epi64x(-999999993LL, 27LL); show("_mm_set_epi64x#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_set1_ps(-1.750f); show("_mm_set1_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_set_ps(-1.750f, 0.500f, 2.750f, 5.000f); show("_mm_set_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_setr_ps(-1.750f, 0.500f, 2.750f, 5.000f); show("_mm_setr_ps#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_set1_pd(-4.500); show("_mm_set1_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_set_pd(-4.500, -1.000); show("_mm_set_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_setr_pd(-4.500, -1.000); show("_mm_setr_pd#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_set_ss(-1.750f); show("_mm_set_ss#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_set_sd(-4.500); show("_mm_set_sd#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_set_ps1(-1.750f); show("_mm_set_ps1#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_set_pd1(-4.500); show("_mm_set_pd1#0", &__v, sizeof(__v)); }
    fill();
    { __m128i __v = _mm_load_si128((__m128i *)buffer); show("_mm_load_si128", &__v, sizeof(__v)); }
    fill();
    { __m128i __v = _mm_loadu_si128((__m128i *)buffer); show("_mm_loadu_si128", &__v, sizeof(__v)); }
    fill(); _mm_store_si128((__m128i *)buffer, i128[1]); show("_mm_store_si128", buffer, 64);
    fill(); _mm_storeu_si128((__m128i *)buffer, i128[1]); show("_mm_storeu_si128", buffer, 64);
    fill(); _mm_stream_si128((__m128i *)buffer, i128[1]); show("_mm_stream_si128", buffer, 64);
    fill();
    { __m128 __v = _mm_load_ps((float *)buffer); show("_mm_load_ps", &__v, sizeof(__v)); }
    fill();
    { __m128 __v = _mm_loadu_ps((float *)buffer); show("_mm_loadu_ps", &__v, sizeof(__v)); }
    fill(); _mm_store_ps((float *)buffer, f128[1]); show("_mm_store_ps", buffer, 64);
    fill(); _mm_storeu_ps((float *)buffer, f128[1]); show("_mm_storeu_ps", buffer, 64);
    fill(); _mm_stream_ps((float *)buffer, f128[1]); show("_mm_stream_ps", buffer, 64);
    fill();
    { __m128d __v = _mm_load_pd((double *)buffer); show("_mm_load_pd", &__v, sizeof(__v)); }
    fill();
    { __m128d __v = _mm_loadu_pd((double *)buffer); show("_mm_loadu_pd", &__v, sizeof(__v)); }
    fill(); _mm_store_pd((double *)buffer, d128[1]); show("_mm_store_pd", buffer, 64);
    fill(); _mm_storeu_pd((double *)buffer, d128[1]); show("_mm_storeu_pd", buffer, 64);
    fill(); _mm_stream_pd((double *)buffer, d128[1]); show("_mm_stream_pd", buffer, 64);
    fill();
    { __m128i __v = _mm_lddqu_si128((__m128i *)buffer); show("_mm_lddqu_si128", &__v, sizeof(__v)); }
    fill();
    { __m128i __v = _mm_loadl_epi64((__m128i *)buffer); show("_mm_loadl_epi64", &__v, sizeof(__v)); }
    fill(); _mm_storel_epi64((__m128i *)buffer, i128[1]); show("_mm_storel_epi64", buffer, 64);
    fill();
    { __m128 __v = _mm_load_ss((float *)buffer); show("_mm_load_ss", &__v, sizeof(__v)); }
    fill();
    { __m128d __v = _mm_load_sd((double *)buffer); show("_mm_load_sd", &__v, sizeof(__v)); }
    fill();
    { __m128 __v = _mm_load1_ps((float *)buffer); show("_mm_load1_ps", &__v, sizeof(__v)); }
    fill();
    { __m128d __v = _mm_load1_pd((double *)buffer); show("_mm_load1_pd", &__v, sizeof(__v)); }
    fill(); _mm_store_ss((float *)buffer, f128[1]); show("_mm_store_ss", buffer, 64);
    fill(); _mm_store_sd((double *)buffer, d128[1]); show("_mm_store_sd", buffer, 64);
    { __m128i __v = _mm_cvtsi32_si128(3); show("_mm_cvtsi32_si128#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtsi64_si128(-999999993LL); show("_mm_cvtsi64_si128#0", &__v, sizeof(__v)); }
    printf("intrinsics: _mm_cvtsi128_si32#0: %lld\n", (long long)(_mm_cvtsi128_si32(i128[0])));
    printf("intrinsics: _mm_cvtsi128_si32#1: %lld\n", (long long)(_mm_cvtsi128_si32(i128[1])));
    printf("intrinsics: _mm_cvtsi128_si64#0: %lld\n", (long long)(_mm_cvtsi128_si64(i128[0])));
    printf("intrinsics: _mm_cvtsi128_si64#1: %lld\n", (long long)(_mm_cvtsi128_si64(i128[1])));
    { float __f = _mm_cvtss_f32(f128[0]); show("_mm_cvtss_f32#0", &__f, 4); }
    { float __f = _mm_cvtss_f32(f128[1]); show("_mm_cvtss_f32#1", &__f, 4); }
    { double __f = _mm_cvtsd_f64(d128[0]); show("_mm_cvtsd_f64#0", &__f, 8); }
    { double __f = _mm_cvtsd_f64(d128[1]); show("_mm_cvtsd_f64#1", &__f, 8); }
    { __m128 __v = _mm_castsi128_ps(i128[0]); show("_mm_castsi128_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_castsi128_ps(i128[1]); show("_mm_castsi128_ps#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_castsi128_pd(i128[0]); show("_mm_castsi128_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_castsi128_pd(i128[1]); show("_mm_castsi128_pd#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_castps_si128(f128[0]); show("_mm_castps_si128#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_castps_si128(f128[1]); show("_mm_castps_si128#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_castpd_si128(d128[0]); show("_mm_castpd_si128#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_castpd_si128(d128[1]); show("_mm_castpd_si128#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_castps_pd(f128[0]); show("_mm_castps_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_castps_pd(f128[1]); show("_mm_castps_pd#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_castpd_ps(d128[0]); show("_mm_castpd_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_castpd_ps(d128[1]); show("_mm_castpd_ps#1", &__v, sizeof(__v)); }
    printf("intrinsics: _mm_extract_epi8#0: %lld\n", (long long)(_mm_extract_epi8(i128[0], 1)));
    printf("intrinsics: _mm_extract_epi16#0: %lld\n", (long long)(_mm_extract_epi16(i128[0], 1)));
    printf("intrinsics: _mm_extract_epi32#0: %lld\n", (long long)(_mm_extract_epi32(i128[0], 1)));
    printf("intrinsics: _mm_extract_epi64#0: %lld\n", (long long)(_mm_extract_epi64(i128[0], 1)));
    { __m128i __v = _mm_insert_epi8(i128[0], -99978, 1); show("_mm_insert_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_insert_epi16(i128[0], -99978, 1); show("_mm_insert_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_insert_epi32(i128[0], -99978, 1); show("_mm_insert_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_insert_epi64(i128[0], 27LL, 1); show("_mm_insert_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_broadcastb_epi8(i128[0]); show("_mm_broadcastb_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_broadcastb_epi8(i128[1]); show("_mm_broadcastb_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_broadcastw_epi16(i128[0]); show("_mm_broadcastw_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_broadcastw_epi16(i128[1]); show("_mm_broadcastw_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_broadcastd_epi32(i128[0]); show("_mm_broadcastd_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_broadcastd_epi32(i128[1]); show("_mm_broadcastd_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_broadcastq_epi64(i128[0]); show("_mm_broadcastq_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_broadcastq_epi64(i128[1]); show("_mm_broadcastq_epi64#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_broadcastss_ps(f128[0]); show("_mm_broadcastss_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_broadcastss_ps(f128[1]); show("_mm_broadcastss_ps#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_broadcastsd_pd(d128[0]); show("_mm_broadcastsd_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_broadcastsd_pd(d128[1]); show("_mm_broadcastsd_pd#1", &__v, sizeof(__v)); }
    fill();
    { __m128 __v = _mm_broadcast_ss((float *)buffer); show("_mm_broadcast_ss", &__v, sizeof(__v)); }
    _mm_pause();
    _mm_mfence();
    _mm_lfence();
    _mm_sfence();
    { __m128i __v = _mm_add_epi8(i128[0], i128[1]); show("_mm_add_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_add_epi8(i128[1], i128[2]); show("_mm_add_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_add_epi16(i128[0], i128[1]); show("_mm_add_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_add_epi16(i128[1], i128[2]); show("_mm_add_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_add_epi32(i128[0], i128[1]); show("_mm_add_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_add_epi32(i128[1], i128[2]); show("_mm_add_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_add_epi64(i128[0], i128[1]); show("_mm_add_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_add_epi64(i128[1], i128[2]); show("_mm_add_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sub_epi8(i128[0], i128[1]); show("_mm_sub_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sub_epi8(i128[1], i128[2]); show("_mm_sub_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sub_epi16(i128[0], i128[1]); show("_mm_sub_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sub_epi16(i128[1], i128[2]); show("_mm_sub_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sub_epi32(i128[0], i128[1]); show("_mm_sub_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sub_epi32(i128[1], i128[2]); show("_mm_sub_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sub_epi64(i128[0], i128[1]); show("_mm_sub_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sub_epi64(i128[1], i128[2]); show("_mm_sub_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_adds_epi8(i128[0], i128[1]); show("_mm_adds_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_adds_epi8(i128[1], i128[2]); show("_mm_adds_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_adds_epi16(i128[0], i128[1]); show("_mm_adds_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_adds_epi16(i128[1], i128[2]); show("_mm_adds_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_adds_epu8(i128[0], i128[1]); show("_mm_adds_epu8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_adds_epu8(i128[1], i128[2]); show("_mm_adds_epu8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_adds_epu16(i128[0], i128[1]); show("_mm_adds_epu16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_adds_epu16(i128[1], i128[2]); show("_mm_adds_epu16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_subs_epi8(i128[0], i128[1]); show("_mm_subs_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_subs_epi8(i128[1], i128[2]); show("_mm_subs_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_subs_epi16(i128[0], i128[1]); show("_mm_subs_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_subs_epi16(i128[1], i128[2]); show("_mm_subs_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_subs_epu8(i128[0], i128[1]); show("_mm_subs_epu8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_subs_epu8(i128[1], i128[2]); show("_mm_subs_epu8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_subs_epu16(i128[0], i128[1]); show("_mm_subs_epu16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_subs_epu16(i128[1], i128[2]); show("_mm_subs_epu16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_mullo_epi16(i128[0], i128[1]); show("_mm_mullo_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_mullo_epi16(i128[1], i128[2]); show("_mm_mullo_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_mullo_epi32(i128[0], i128[1]); show("_mm_mullo_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_mullo_epi32(i128[1], i128[2]); show("_mm_mullo_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_mulhi_epi16(i128[0], i128[1]); show("_mm_mulhi_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_mulhi_epi16(i128[1], i128[2]); show("_mm_mulhi_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_mulhi_epu16(i128[0], i128[1]); show("_mm_mulhi_epu16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_mulhi_epu16(i128[1], i128[2]); show("_mm_mulhi_epu16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_mul_epi32(i128[0], i128[1]); show("_mm_mul_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_mul_epi32(i128[1], i128[2]); show("_mm_mul_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_mul_epu32(i128[0], i128[1]); show("_mm_mul_epu32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_mul_epu32(i128[1], i128[2]); show("_mm_mul_epu32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_madd_epi16(i128[0], i128[1]); show("_mm_madd_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_madd_epi16(i128[1], i128[2]); show("_mm_madd_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_maddubs_epi16(i128[0], i128[1]); show("_mm_maddubs_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_maddubs_epi16(i128[1], i128[2]); show("_mm_maddubs_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sad_epu8(i128[0], i128[1]); show("_mm_sad_epu8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sad_epu8(i128[1], i128[2]); show("_mm_sad_epu8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_avg_epu8(i128[0], i128[1]); show("_mm_avg_epu8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_avg_epu8(i128[1], i128[2]); show("_mm_avg_epu8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_avg_epu16(i128[0], i128[1]); show("_mm_avg_epu16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_avg_epu16(i128[1], i128[2]); show("_mm_avg_epu16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_mulhrs_epi16(i128[0], i128[1]); show("_mm_mulhrs_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_mulhrs_epi16(i128[1], i128[2]); show("_mm_mulhrs_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_min_epi8(i128[0], i128[1]); show("_mm_min_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_min_epi8(i128[1], i128[2]); show("_mm_min_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_min_epi16(i128[0], i128[1]); show("_mm_min_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_min_epi16(i128[1], i128[2]); show("_mm_min_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_min_epi32(i128[0], i128[1]); show("_mm_min_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_min_epi32(i128[1], i128[2]); show("_mm_min_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_min_epu8(i128[0], i128[1]); show("_mm_min_epu8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_min_epu8(i128[1], i128[2]); show("_mm_min_epu8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_min_epu16(i128[0], i128[1]); show("_mm_min_epu16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_min_epu16(i128[1], i128[2]); show("_mm_min_epu16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_min_epu32(i128[0], i128[1]); show("_mm_min_epu32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_min_epu32(i128[1], i128[2]); show("_mm_min_epu32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_max_epi8(i128[0], i128[1]); show("_mm_max_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_max_epi8(i128[1], i128[2]); show("_mm_max_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_max_epi16(i128[0], i128[1]); show("_mm_max_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_max_epi16(i128[1], i128[2]); show("_mm_max_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_max_epi32(i128[0], i128[1]); show("_mm_max_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_max_epi32(i128[1], i128[2]); show("_mm_max_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_max_epu8(i128[0], i128[1]); show("_mm_max_epu8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_max_epu8(i128[1], i128[2]); show("_mm_max_epu8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_max_epu16(i128[0], i128[1]); show("_mm_max_epu16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_max_epu16(i128[1], i128[2]); show("_mm_max_epu16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_max_epu32(i128[0], i128[1]); show("_mm_max_epu32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_max_epu32(i128[1], i128[2]); show("_mm_max_epu32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sign_epi8(i128[0], i128[1]); show("_mm_sign_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sign_epi8(i128[1], i128[2]); show("_mm_sign_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sign_epi16(i128[0], i128[1]); show("_mm_sign_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sign_epi16(i128[1], i128[2]); show("_mm_sign_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sign_epi32(i128[0], i128[1]); show("_mm_sign_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sign_epi32(i128[1], i128[2]); show("_mm_sign_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_hadd_epi16(i128[0], i128[1]); show("_mm_hadd_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_hadd_epi16(i128[1], i128[2]); show("_mm_hadd_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_hadd_epi32(i128[0], i128[1]); show("_mm_hadd_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_hadd_epi32(i128[1], i128[2]); show("_mm_hadd_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_hadds_epi16(i128[0], i128[1]); show("_mm_hadds_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_hadds_epi16(i128[1], i128[2]); show("_mm_hadds_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_hsub_epi16(i128[0], i128[1]); show("_mm_hsub_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_hsub_epi16(i128[1], i128[2]); show("_mm_hsub_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_hsub_epi32(i128[0], i128[1]); show("_mm_hsub_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_hsub_epi32(i128[1], i128[2]); show("_mm_hsub_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_hsubs_epi16(i128[0], i128[1]); show("_mm_hsubs_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_hsubs_epi16(i128[1], i128[2]); show("_mm_hsubs_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_and_si128(i128[0], i128[1]); show("_mm_and_si128#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_and_si128(i128[1], i128[2]); show("_mm_and_si128#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_or_si128(i128[0], i128[1]); show("_mm_or_si128#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_or_si128(i128[1], i128[2]); show("_mm_or_si128#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_xor_si128(i128[0], i128[1]); show("_mm_xor_si128#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_xor_si128(i128[1], i128[2]); show("_mm_xor_si128#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_andnot_si128(i128[0], i128[1]); show("_mm_andnot_si128#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_andnot_si128(i128[1], i128[2]); show("_mm_andnot_si128#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpeq_epi8(i128[0], i128[1]); show("_mm_cmpeq_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpeq_epi8(i128[1], i128[2]); show("_mm_cmpeq_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpeq_epi16(i128[0], i128[1]); show("_mm_cmpeq_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpeq_epi16(i128[1], i128[2]); show("_mm_cmpeq_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpeq_epi32(i128[0], i128[1]); show("_mm_cmpeq_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpeq_epi32(i128[1], i128[2]); show("_mm_cmpeq_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpeq_epi64(i128[0], i128[1]); show("_mm_cmpeq_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpeq_epi64(i128[1], i128[2]); show("_mm_cmpeq_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpgt_epi8(i128[0], i128[1]); show("_mm_cmpgt_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpgt_epi8(i128[1], i128[2]); show("_mm_cmpgt_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpgt_epi16(i128[0], i128[1]); show("_mm_cmpgt_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpgt_epi16(i128[1], i128[2]); show("_mm_cmpgt_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpgt_epi32(i128[0], i128[1]); show("_mm_cmpgt_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpgt_epi32(i128[1], i128[2]); show("_mm_cmpgt_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpgt_epi64(i128[0], i128[1]); show("_mm_cmpgt_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmpgt_epi64(i128[1], i128[2]); show("_mm_cmpgt_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_shuffle_epi8(i128[0], i128[1]); show("_mm_shuffle_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_shuffle_epi8(i128[1], i128[2]); show("_mm_shuffle_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpacklo_epi8(i128[0], i128[1]); show("_mm_unpacklo_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpacklo_epi8(i128[1], i128[2]); show("_mm_unpacklo_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpacklo_epi16(i128[0], i128[1]); show("_mm_unpacklo_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpacklo_epi16(i128[1], i128[2]); show("_mm_unpacklo_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpacklo_epi32(i128[0], i128[1]); show("_mm_unpacklo_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpacklo_epi32(i128[1], i128[2]); show("_mm_unpacklo_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpacklo_epi64(i128[0], i128[1]); show("_mm_unpacklo_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpacklo_epi64(i128[1], i128[2]); show("_mm_unpacklo_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpackhi_epi8(i128[0], i128[1]); show("_mm_unpackhi_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpackhi_epi8(i128[1], i128[2]); show("_mm_unpackhi_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpackhi_epi16(i128[0], i128[1]); show("_mm_unpackhi_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpackhi_epi16(i128[1], i128[2]); show("_mm_unpackhi_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpackhi_epi32(i128[0], i128[1]); show("_mm_unpackhi_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpackhi_epi32(i128[1], i128[2]); show("_mm_unpackhi_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpackhi_epi64(i128[0], i128[1]); show("_mm_unpackhi_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_unpackhi_epi64(i128[1], i128[2]); show("_mm_unpackhi_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_packs_epi16(i128[0], i128[1]); show("_mm_packs_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_packs_epi16(i128[1], i128[2]); show("_mm_packs_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_packs_epi32(i128[0], i128[1]); show("_mm_packs_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_packs_epi32(i128[1], i128[2]); show("_mm_packs_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_packus_epi16(i128[0], i128[1]); show("_mm_packus_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_packus_epi16(i128[1], i128[2]); show("_mm_packus_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_packus_epi32(i128[0], i128[1]); show("_mm_packus_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_packus_epi32(i128[1], i128[2]); show("_mm_packus_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmplt_epi8(i128[0], i128[1]); show("_mm_cmplt_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmplt_epi8(i128[1], i128[2]); show("_mm_cmplt_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmplt_epi16(i128[0], i128[1]); show("_mm_cmplt_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmplt_epi16(i128[1], i128[2]); show("_mm_cmplt_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmplt_epi32(i128[0], i128[1]); show("_mm_cmplt_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cmplt_epi32(i128[1], i128[2]); show("_mm_cmplt_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_slli_epi16(i128[0], 3); show("_mm_slli_epi16#0/3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_slli_epi16(i128[0], 31); show("_mm_slli_epi16#0/31", &__v, sizeof(__v)); }
    { __m128i __v = _mm_slli_epi16(i128[0], 33); show("_mm_slli_epi16#0/33", &__v, sizeof(__v)); }
    { __m128i __v = _mm_slli_epi32(i128[0], 3); show("_mm_slli_epi32#0/3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_slli_epi32(i128[0], 31); show("_mm_slli_epi32#0/31", &__v, sizeof(__v)); }
    { __m128i __v = _mm_slli_epi32(i128[0], 33); show("_mm_slli_epi32#0/33", &__v, sizeof(__v)); }
    { __m128i __v = _mm_slli_epi64(i128[0], 3); show("_mm_slli_epi64#0/3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_slli_epi64(i128[0], 31); show("_mm_slli_epi64#0/31", &__v, sizeof(__v)); }
    { __m128i __v = _mm_slli_epi64(i128[0], 33); show("_mm_slli_epi64#0/33", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srli_epi16(i128[0], 3); show("_mm_srli_epi16#0/3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srli_epi16(i128[0], 31); show("_mm_srli_epi16#0/31", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srli_epi16(i128[0], 33); show("_mm_srli_epi16#0/33", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srli_epi32(i128[0], 3); show("_mm_srli_epi32#0/3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srli_epi32(i128[0], 31); show("_mm_srli_epi32#0/31", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srli_epi32(i128[0], 33); show("_mm_srli_epi32#0/33", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srli_epi64(i128[0], 3); show("_mm_srli_epi64#0/3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srli_epi64(i128[0], 31); show("_mm_srli_epi64#0/31", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srli_epi64(i128[0], 33); show("_mm_srli_epi64#0/33", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srai_epi16(i128[0], 3); show("_mm_srai_epi16#0/3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srai_epi16(i128[0], 31); show("_mm_srai_epi16#0/31", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srai_epi16(i128[0], 33); show("_mm_srai_epi16#0/33", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srai_epi32(i128[0], 3); show("_mm_srai_epi32#0/3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srai_epi32(i128[0], 31); show("_mm_srai_epi32#0/31", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srai_epi32(i128[0], 33); show("_mm_srai_epi32#0/33", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sll_epi16(i128[0], i128[1]); show("_mm_sll_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sll_epi16(i128[1], i128[2]); show("_mm_sll_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sll_epi32(i128[0], i128[1]); show("_mm_sll_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sll_epi32(i128[1], i128[2]); show("_mm_sll_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sll_epi64(i128[0], i128[1]); show("_mm_sll_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sll_epi64(i128[1], i128[2]); show("_mm_sll_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srl_epi16(i128[0], i128[1]); show("_mm_srl_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srl_epi16(i128[1], i128[2]); show("_mm_srl_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srl_epi32(i128[0], i128[1]); show("_mm_srl_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srl_epi32(i128[1], i128[2]); show("_mm_srl_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srl_epi64(i128[0], i128[1]); show("_mm_srl_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srl_epi64(i128[1], i128[2]); show("_mm_srl_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sra_epi16(i128[0], i128[1]); show("_mm_sra_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sra_epi16(i128[1], i128[2]); show("_mm_sra_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sra_epi32(i128[0], i128[1]); show("_mm_sra_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_sra_epi32(i128[1], i128[2]); show("_mm_sra_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_abs_epi8(i128[0]); show("_mm_abs_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_abs_epi8(i128[1]); show("_mm_abs_epi8#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_abs_epi16(i128[0]); show("_mm_abs_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_abs_epi16(i128[1]); show("_mm_abs_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_abs_epi32(i128[0]); show("_mm_abs_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_abs_epi32(i128[1]); show("_mm_abs_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepi8_epi16(i128[0]); show("_mm_cvtepi8_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepi8_epi16(i128[1]); show("_mm_cvtepi8_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepi8_epi32(i128[0]); show("_mm_cvtepi8_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepi8_epi32(i128[1]); show("_mm_cvtepi8_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepi8_epi64(i128[0]); show("_mm_cvtepi8_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepi8_epi64(i128[1]); show("_mm_cvtepi8_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepi16_epi32(i128[0]); show("_mm_cvtepi16_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepi16_epi32(i128[1]); show("_mm_cvtepi16_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepi16_epi64(i128[0]); show("_mm_cvtepi16_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepi16_epi64(i128[1]); show("_mm_cvtepi16_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepi32_epi64(i128[0]); show("_mm_cvtepi32_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepi32_epi64(i128[1]); show("_mm_cvtepi32_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepu8_epi16(i128[0]); show("_mm_cvtepu8_epi16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepu8_epi16(i128[1]); show("_mm_cvtepu8_epi16#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepu8_epi32(i128[0]); show("_mm_cvtepu8_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepu8_epi32(i128[1]); show("_mm_cvtepu8_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepu8_epi64(i128[0]); show("_mm_cvtepu8_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepu8_epi64(i128[1]); show("_mm_cvtepu8_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepu16_epi32(i128[0]); show("_mm_cvtepu16_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepu16_epi32(i128[1]); show("_mm_cvtepu16_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepu16_epi64(i128[0]); show("_mm_cvtepu16_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepu16_epi64(i128[1]); show("_mm_cvtepu16_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepu32_epi64(i128[0]); show("_mm_cvtepu32_epi64#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtepu32_epi64(i128[1]); show("_mm_cvtepu32_epi64#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_minpos_epu16(i128[0]); show("_mm_minpos_epu16#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_minpos_epu16(i128[1]); show("_mm_minpos_epu16#1", &__v, sizeof(__v)); }
    printf("intrinsics: _mm_movemask_epi8#0: %lld\n", (long long)(_mm_movemask_epi8(i128[0])));
    printf("intrinsics: _mm_movemask_epi8#1: %lld\n", (long long)(_mm_movemask_epi8(i128[1])));
    printf("intrinsics: _mm_movemask_ps#0: %lld\n", (long long)(_mm_movemask_ps(f128[0])));
    printf("intrinsics: _mm_movemask_ps#1: %lld\n", (long long)(_mm_movemask_ps(f128[1])));
    printf("intrinsics: _mm_movemask_pd#0: %lld\n", (long long)(_mm_movemask_pd(d128[0])));
    printf("intrinsics: _mm_movemask_pd#1: %lld\n", (long long)(_mm_movemask_pd(d128[1])));
    printf("intrinsics: _mm_testz_si128#0: %lld\n", (long long)(_mm_testz_si128(i128[0], i128[1])));
    printf("intrinsics: _mm_testz_si128#1: %lld\n", (long long)(_mm_testz_si128(i128[1], i128[2])));
    printf("intrinsics: _mm_testc_si128#0: %lld\n", (long long)(_mm_testc_si128(i128[0], i128[1])));
    printf("intrinsics: _mm_testc_si128#1: %lld\n", (long long)(_mm_testc_si128(i128[1], i128[2])));
    printf("intrinsics: _mm_testnzc_si128#0: %lld\n", (long long)(_mm_testnzc_si128(i128[0], i128[1])));
    printf("intrinsics: _mm_testnzc_si128#1: %lld\n", (long long)(_mm_testnzc_si128(i128[1], i128[2])));
    { __m128i __v = _mm_blendv_epi8(i128[0], i128[1], i128[2]); show("_mm_blendv_epi8#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_blendv_epi8(i128[1], i128[2], i128[0]); show("_mm_blendv_epi8#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_add_ps(f128[0], f128[1]); show("_mm_add_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_add_ps(f128[1], f128[2]); show("_mm_add_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_sub_ps(f128[0], f128[1]); show("_mm_sub_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_sub_ps(f128[1], f128[2]); show("_mm_sub_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_mul_ps(f128[0], f128[1]); show("_mm_mul_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_mul_ps(f128[1], f128[2]); show("_mm_mul_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_div_ps(f128[0], f128[1]); show("_mm_div_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_div_ps(f128[1], f128[2]); show("_mm_div_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_min_ps(f128[0], f128[1]); show("_mm_min_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_min_ps(f128[1], f128[2]); show("_mm_min_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_max_ps(f128[0], f128[1]); show("_mm_max_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_max_ps(f128[1], f128[2]); show("_mm_max_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_and_ps(f128[0], f128[1]); show("_mm_and_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_and_ps(f128[1], f128[2]); show("_mm_and_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_or_ps(f128[0], f128[1]); show("_mm_or_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_or_ps(f128[1], f128[2]); show("_mm_or_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_xor_ps(f128[0], f128[1]); show("_mm_xor_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_xor_ps(f128[1], f128[2]); show("_mm_xor_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_andnot_ps(f128[0], f128[1]); show("_mm_andnot_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_andnot_ps(f128[1], f128[2]); show("_mm_andnot_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_unpacklo_ps(f128[0], f128[1]); show("_mm_unpacklo_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_unpacklo_ps(f128[1], f128[2]); show("_mm_unpacklo_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_unpackhi_ps(f128[0], f128[1]); show("_mm_unpackhi_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_unpackhi_ps(f128[1], f128[2]); show("_mm_unpackhi_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_hadd_ps(f128[0], f128[1]); show("_mm_hadd_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_hadd_ps(f128[1], f128[2]); show("_mm_hadd_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_hsub_ps(f128[0], f128[1]); show("_mm_hsub_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_hsub_ps(f128[1], f128[2]); show("_mm_hsub_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_addsub_ps(f128[0], f128[1]); show("_mm_addsub_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_addsub_ps(f128[1], f128[2]); show("_mm_addsub_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_sqrt_ps(f128[0]); show("_mm_sqrt_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_sqrt_ps(f128[1]); show("_mm_sqrt_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_blendv_ps(f128[0], f128[1], f128[2]); show("_mm_blendv_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_blendv_ps(f128[1], f128[2], f128[0]); show("_mm_blendv_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpeq_ps(f128[0], f128[1]); show("_mm_cmpeq_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpeq_ps(f128[1], f128[2]); show("_mm_cmpeq_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmplt_ps(f128[0], f128[1]); show("_mm_cmplt_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmplt_ps(f128[1], f128[2]); show("_mm_cmplt_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmple_ps(f128[0], f128[1]); show("_mm_cmple_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmple_ps(f128[1], f128[2]); show("_mm_cmple_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpunord_ps(f128[0], f128[1]); show("_mm_cmpunord_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpunord_ps(f128[1], f128[2]); show("_mm_cmpunord_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpneq_ps(f128[0], f128[1]); show("_mm_cmpneq_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpneq_ps(f128[1], f128[2]); show("_mm_cmpneq_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpnlt_ps(f128[0], f128[1]); show("_mm_cmpnlt_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpnlt_ps(f128[1], f128[2]); show("_mm_cmpnlt_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpnle_ps(f128[0], f128[1]); show("_mm_cmpnle_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpnle_ps(f128[1], f128[2]); show("_mm_cmpnle_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpord_ps(f128[0], f128[1]); show("_mm_cmpord_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpord_ps(f128[1], f128[2]); show("_mm_cmpord_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpgt_ps(f128[0], f128[1]); show("_mm_cmpgt_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpgt_ps(f128[1], f128[2]); show("_mm_cmpgt_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpge_ps(f128[0], f128[1]); show("_mm_cmpge_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmpge_ps(f128[1], f128[2]); show("_mm_cmpge_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_add_ss(f128[0], f128[1]); show("_mm_add_ss#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_add_ss(f128[1], f128[2]); show("_mm_add_ss#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_sub_ss(f128[0], f128[1]); show("_mm_sub_ss#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_sub_ss(f128[1], f128[2]); show("_mm_sub_ss#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_mul_ss(f128[0], f128[1]); show("_mm_mul_ss#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_mul_ss(f128[1], f128[2]); show("_mm_mul_ss#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_div_ss(f128[0], f128[1]); show("_mm_div_ss#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_div_ss(f128[1], f128[2]); show("_mm_div_ss#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_min_ss(f128[0], f128[1]); show("_mm_min_ss#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_min_ss(f128[1], f128[2]); show("_mm_min_ss#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_max_ss(f128[0], f128[1]); show("_mm_max_ss#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_max_ss(f128[1], f128[2]); show("_mm_max_ss#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_add_pd(d128[0], d128[1]); show("_mm_add_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_add_pd(d128[1], d128[2]); show("_mm_add_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_sub_pd(d128[0], d128[1]); show("_mm_sub_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_sub_pd(d128[1], d128[2]); show("_mm_sub_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_mul_pd(d128[0], d128[1]); show("_mm_mul_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_mul_pd(d128[1], d128[2]); show("_mm_mul_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_div_pd(d128[0], d128[1]); show("_mm_div_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_div_pd(d128[1], d128[2]); show("_mm_div_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_min_pd(d128[0], d128[1]); show("_mm_min_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_min_pd(d128[1], d128[2]); show("_mm_min_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_max_pd(d128[0], d128[1]); show("_mm_max_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_max_pd(d128[1], d128[2]); show("_mm_max_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_and_pd(d128[0], d128[1]); show("_mm_and_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_and_pd(d128[1], d128[2]); show("_mm_and_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_or_pd(d128[0], d128[1]); show("_mm_or_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_or_pd(d128[1], d128[2]); show("_mm_or_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_xor_pd(d128[0], d128[1]); show("_mm_xor_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_xor_pd(d128[1], d128[2]); show("_mm_xor_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_andnot_pd(d128[0], d128[1]); show("_mm_andnot_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_andnot_pd(d128[1], d128[2]); show("_mm_andnot_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_unpacklo_pd(d128[0], d128[1]); show("_mm_unpacklo_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_unpacklo_pd(d128[1], d128[2]); show("_mm_unpacklo_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_unpackhi_pd(d128[0], d128[1]); show("_mm_unpackhi_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_unpackhi_pd(d128[1], d128[2]); show("_mm_unpackhi_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_hadd_pd(d128[0], d128[1]); show("_mm_hadd_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_hadd_pd(d128[1], d128[2]); show("_mm_hadd_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_hsub_pd(d128[0], d128[1]); show("_mm_hsub_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_hsub_pd(d128[1], d128[2]); show("_mm_hsub_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_addsub_pd(d128[0], d128[1]); show("_mm_addsub_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_addsub_pd(d128[1], d128[2]); show("_mm_addsub_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_sqrt_pd(d128[0]); show("_mm_sqrt_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_sqrt_pd(d128[1]); show("_mm_sqrt_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_blendv_pd(d128[0], d128[1], d128[2]); show("_mm_blendv_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_blendv_pd(d128[1], d128[2], d128[0]); show("_mm_blendv_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpeq_pd(d128[0], d128[1]); show("_mm_cmpeq_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpeq_pd(d128[1], d128[2]); show("_mm_cmpeq_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmplt_pd(d128[0], d128[1]); show("_mm_cmplt_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmplt_pd(d128[1], d128[2]); show("_mm_cmplt_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmple_pd(d128[0], d128[1]); show("_mm_cmple_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmple_pd(d128[1], d128[2]); show("_mm_cmple_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpunord_pd(d128[0], d128[1]); show("_mm_cmpunord_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpunord_pd(d128[1], d128[2]); show("_mm_cmpunord_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpneq_pd(d128[0], d128[1]); show("_mm_cmpneq_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpneq_pd(d128[1], d128[2]); show("_mm_cmpneq_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpnlt_pd(d128[0], d128[1]); show("_mm_cmpnlt_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpnlt_pd(d128[1], d128[2]); show("_mm_cmpnlt_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpnle_pd(d128[0], d128[1]); show("_mm_cmpnle_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpnle_pd(d128[1], d128[2]); show("_mm_cmpnle_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpord_pd(d128[0], d128[1]); show("_mm_cmpord_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpord_pd(d128[1], d128[2]); show("_mm_cmpord_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpgt_pd(d128[0], d128[1]); show("_mm_cmpgt_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpgt_pd(d128[1], d128[2]); show("_mm_cmpgt_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpge_pd(d128[0], d128[1]); show("_mm_cmpge_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmpge_pd(d128[1], d128[2]); show("_mm_cmpge_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_add_sd(d128[0], d128[1]); show("_mm_add_sd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_add_sd(d128[1], d128[2]); show("_mm_add_sd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_sub_sd(d128[0], d128[1]); show("_mm_sub_sd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_sub_sd(d128[1], d128[2]); show("_mm_sub_sd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_mul_sd(d128[0], d128[1]); show("_mm_mul_sd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_mul_sd(d128[1], d128[2]); show("_mm_mul_sd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_div_sd(d128[0], d128[1]); show("_mm_div_sd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_div_sd(d128[1], d128[2]); show("_mm_div_sd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_min_sd(d128[0], d128[1]); show("_mm_min_sd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_min_sd(d128[1], d128[2]); show("_mm_min_sd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_max_sd(d128[0], d128[1]); show("_mm_max_sd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_max_sd(d128[1], d128[2]); show("_mm_max_sd#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_movehl_ps(f128[0], f128[1]); show("_mm_movehl_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_movehl_ps(f128[1], f128[2]); show("_mm_movehl_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_movelh_ps(f128[0], f128[1]); show("_mm_movelh_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_movelh_ps(f128[1], f128[2]); show("_mm_movelh_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_rcp_ps(f128[0]); show("_mm_rcp_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_rcp_ps(f128[1]); show("_mm_rcp_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_rsqrt_ps(f128[0]); show("_mm_rsqrt_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_rsqrt_ps(f128[1]); show("_mm_rsqrt_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_movehdup_ps(f128[0]); show("_mm_movehdup_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_movehdup_ps(f128[1]); show("_mm_movehdup_ps#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_moveldup_ps(f128[0]); show("_mm_moveldup_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_moveldup_ps(f128[1]); show("_mm_moveldup_ps#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_movedup_pd(d128[0]); show("_mm_movedup_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_movedup_pd(d128[1]); show("_mm_movedup_pd#1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cvtepi32_ps(i128[0]); show("_mm_cvtepi32_ps#0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cvtepi32_ps(i128[1]); show("_mm_cvtepi32_ps#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtps_epi32(f128[0]); show("_mm_cvtps_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvtps_epi32(f128[1]); show("_mm_cvtps_epi32#1", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvttps_epi32(f128[0]); show("_mm_cvttps_epi32#0", &__v, sizeof(__v)); }
    { __m128i __v = _mm_cvttps_epi32(f128[1]); show("_mm_cvttps_epi32#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cvtepi32_pd(i128[0]); show("_mm_cvtepi32_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cvtepi32_pd(i128[1]); show("_mm_cvtepi32_pd#1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cvtps_pd(f128[0]); show("_mm_cvtps_pd#0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cvtps_pd(f128[1]); show("_mm_cvtps_pd#1", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_shuffle_epi32(i256[0], 0x3); show("_mm256_shuffle_epi32/0x3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_shuffle_epi32(i256[0], 0x1b); show("_mm256_shuffle_epi32/0x1b", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_shuffle_epi32(i256[0], 0x93); show("_mm256_shuffle_epi32/0x93", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_shufflehi_epi16(i256[0], 0x3); show("_mm256_shufflehi_epi16/0x3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_shufflehi_epi16(i256[0], 0x1b); show("_mm256_shufflehi_epi16/0x1b", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_shufflehi_epi16(i256[0], 0x93); show("_mm256_shufflehi_epi16/0x93", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_shufflelo_epi16(i256[0], 0x3); show("_mm256_shufflelo_epi16/0x3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_shufflelo_epi16(i256[0], 0x1b); show("_mm256_shufflelo_epi16/0x1b", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_shufflelo_epi16(i256[0], 0x93); show("_mm256_shufflelo_epi16/0x93", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_permute4x64_epi64(i256[0], 0x3); show("_mm256_permute4x64_epi64/0x3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_permute4x64_epi64(i256[0], 0x1b); show("_mm256_permute4x64_epi64/0x1b", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_permute4x64_epi64(i256[0], 0x93); show("_mm256_permute4x64_epi64/0x93", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_slli_si256(i256[0], 0x3); show("_mm256_slli_si256/0x3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_slli_si256(i256[0], 0x1b); show("_mm256_slli_si256/0x1b", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_slli_si256(i256[0], 0x93); show("_mm256_slli_si256/0x93", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srli_si256(i256[0], 0x3); show("_mm256_srli_si256/0x3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srli_si256(i256[0], 0x1b); show("_mm256_srli_si256/0x1b", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_srli_si256(i256[0], 0x93); show("_mm256_srli_si256/0x93", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_alignr_epi8(i256[0], i256[1], 0x3); show("_mm256_alignr_epi8/0x3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_alignr_epi8(i256[0], i256[1], 0x1b); show("_mm256_alignr_epi8/0x1b", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_alignr_epi8(i256[0], i256[1], 0x93); show("_mm256_alignr_epi8/0x93", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_blend_epi16(i256[0], i256[1], 0x3); show("_mm256_blend_epi16/0x3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_blend_epi16(i256[0], i256[1], 0x1b); show("_mm256_blend_epi16/0x1b", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_blend_epi16(i256[0], i256[1], 0x93); show("_mm256_blend_epi16/0x93", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_blend_epi32(i256[0], i256[1], 0x3); show("_mm256_blend_epi32/0x3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_blend_epi32(i256[0], i256[1], 0x1b); show("_mm256_blend_epi32/0x1b", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_blend_epi32(i256[0], i256[1], 0x93); show("_mm256_blend_epi32/0x93", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_permute2x128_si256(i256[0], i256[1], 0x8); show("_mm256_permute2x128_si256/0x8", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_permute2x128_si256(i256[0], i256[1], 0x13); show("_mm256_permute2x128_si256/0x13", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_permute2x128_si256(i256[0], i256[1], 0x21); show("_mm256_permute2x128_si256/0x21", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_permute2f128_si256(i256[0], i256[1], 0x8); show("_mm256_permute2f128_si256/0x8", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_permute2f128_si256(i256[0], i256[1], 0x13); show("_mm256_permute2f128_si256/0x13", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_permute2f128_si256(i256[0], i256[1], 0x21); show("_mm256_permute2f128_si256/0x21", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mpsadbw_epu8(i256[0], i256[1], 0x3); show("_mm256_mpsadbw_epu8/0x3", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mpsadbw_epu8(i256[0], i256[1], 0x1b); show("_mm256_mpsadbw_epu8/0x1b", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_mpsadbw_epu8(i256[0], i256[1], 0x93); show("_mm256_mpsadbw_epu8/0x93", &__v, sizeof(__v)); }
    { __m128i __v = _mm_blend_epi32(i128[0], i128[1], 0x3); show("_mm_blend_epi32/0x3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_blend_epi32(i128[0], i128[1], 0xb); show("_mm_blend_epi32/0xb", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_permute_ps(f256[0], 0x3); show("_mm256_permute_ps/0x3", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_permute_ps(f256[0], 0x1b); show("_mm256_permute_ps/0x1b", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_permute_ps(f256[0], 0x93); show("_mm256_permute_ps/0x93", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_round_ps(f256[0], 0x1); show("_mm256_round_ps/0x1", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_round_ps(f256[0], 0x2); show("_mm256_round_ps/0x2", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_round_ps(f256[0], 0x3); show("_mm256_round_ps/0x3", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_round_ps(f256[0], 0x4); show("_mm256_round_ps/0x4", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_shuffle_ps(f256[0], f256[1], 0x3); show("_mm256_shuffle_ps/0x3", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_shuffle_ps(f256[0], f256[1], 0x1b); show("_mm256_shuffle_ps/0x1b", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_shuffle_ps(f256[0], f256[1], 0x93); show("_mm256_shuffle_ps/0x93", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_blend_ps(f256[0], f256[1], 0x3); show("_mm256_blend_ps/0x3", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_blend_ps(f256[0], f256[1], 0x1b); show("_mm256_blend_ps/0x1b", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_blend_ps(f256[0], f256[1], 0x93); show("_mm256_blend_ps/0x93", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_cmp_ps(f256[0], f256[1], 0x0); show("_mm256_cmp_ps/0x0", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_cmp_ps(f256[0], f256[1], 0x5); show("_mm256_cmp_ps/0x5", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_cmp_ps(f256[0], f256[1], 0x11); show("_mm256_cmp_ps/0x11", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_cmp_ps(f256[0], f256[1], 0x1e); show("_mm256_cmp_ps/0x1e", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_permute2f128_ps(f256[0], f256[1], 0x8); show("_mm256_permute2f128_ps/0x8", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_permute2f128_ps(f256[0], f256[1], 0x13); show("_mm256_permute2f128_ps/0x13", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_permute2f128_ps(f256[0], f256[1], 0x21); show("_mm256_permute2f128_ps/0x21", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmp_ps(f128[0], f128[1], 0x0); show("_mm_cmp_ps/0x0", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmp_ps(f128[0], f128[1], 0x5); show("_mm_cmp_ps/0x5", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmp_ps(f128[0], f128[1], 0x11); show("_mm_cmp_ps/0x11", &__v, sizeof(__v)); }
    { __m128 __v = _mm_cmp_ps(f128[0], f128[1], 0x1e); show("_mm_cmp_ps/0x1e", &__v, sizeof(__v)); }
    { __m128 __v = _mm_permute_ps(f128[0], 0x3); show("_mm_permute_ps/0x3", &__v, sizeof(__v)); }
    { __m128 __v = _mm_permute_ps(f128[0], 0x1b); show("_mm_permute_ps/0x1b", &__v, sizeof(__v)); }
    { __m128 __v = _mm_permute_ps(f128[0], 0x93); show("_mm_permute_ps/0x93", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_permute_pd(d256[0], 0x3); show("_mm256_permute_pd/0x3", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_permute_pd(d256[0], 0xb); show("_mm256_permute_pd/0xb", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_round_pd(d256[0], 0x1); show("_mm256_round_pd/0x1", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_round_pd(d256[0], 0x2); show("_mm256_round_pd/0x2", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_round_pd(d256[0], 0x3); show("_mm256_round_pd/0x3", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_round_pd(d256[0], 0x4); show("_mm256_round_pd/0x4", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_shuffle_pd(d256[0], d256[1], 0x3); show("_mm256_shuffle_pd/0x3", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_shuffle_pd(d256[0], d256[1], 0xb); show("_mm256_shuffle_pd/0xb", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_blend_pd(d256[0], d256[1], 0x3); show("_mm256_blend_pd/0x3", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_blend_pd(d256[0], d256[1], 0xb); show("_mm256_blend_pd/0xb", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_cmp_pd(d256[0], d256[1], 0x0); show("_mm256_cmp_pd/0x0", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_cmp_pd(d256[0], d256[1], 0x5); show("_mm256_cmp_pd/0x5", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_cmp_pd(d256[0], d256[1], 0x11); show("_mm256_cmp_pd/0x11", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_cmp_pd(d256[0], d256[1], 0x1e); show("_mm256_cmp_pd/0x1e", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_permute2f128_pd(d256[0], d256[1], 0x8); show("_mm256_permute2f128_pd/0x8", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_permute2f128_pd(d256[0], d256[1], 0x13); show("_mm256_permute2f128_pd/0x13", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_permute2f128_pd(d256[0], d256[1], 0x21); show("_mm256_permute2f128_pd/0x21", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmp_pd(d128[0], d128[1], 0x0); show("_mm_cmp_pd/0x0", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmp_pd(d128[0], d128[1], 0x5); show("_mm_cmp_pd/0x5", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmp_pd(d128[0], d128[1], 0x11); show("_mm_cmp_pd/0x11", &__v, sizeof(__v)); }
    { __m128d __v = _mm_cmp_pd(d128[0], d128[1], 0x1e); show("_mm_cmp_pd/0x1e", &__v, sizeof(__v)); }
    { __m128d __v = _mm_permute_pd(d128[0], 0x3); show("_mm_permute_pd/0x3", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_dp_ps(f256[0], f256[1], 0x3); show("_mm256_dp_ps/0x3", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_dp_ps(f256[0], f256[1], 0x1b); show("_mm256_dp_ps/0x1b", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_dp_ps(f256[0], f256[1], 0x93); show("_mm256_dp_ps/0x93", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_permute4x64_pd(d256[0], 0x3); show("_mm256_permute4x64_pd/0x3", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_permute4x64_pd(d256[0], 0x1b); show("_mm256_permute4x64_pd/0x1b", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_permute4x64_pd(d256[0], 0x93); show("_mm256_permute4x64_pd/0x93", &__v, sizeof(__v)); }
    { __m128i __v = _mm_shuffle_epi32(i128[0], 0x3); show("_mm_shuffle_epi32/0x3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_shuffle_epi32(i128[0], 0x1b); show("_mm_shuffle_epi32/0x1b", &__v, sizeof(__v)); }
    { __m128i __v = _mm_shuffle_epi32(i128[0], 0x93); show("_mm_shuffle_epi32/0x93", &__v, sizeof(__v)); }
    { __m128i __v = _mm_shufflehi_epi16(i128[0], 0x3); show("_mm_shufflehi_epi16/0x3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_shufflehi_epi16(i128[0], 0x1b); show("_mm_shufflehi_epi16/0x1b", &__v, sizeof(__v)); }
    { __m128i __v = _mm_shufflehi_epi16(i128[0], 0x93); show("_mm_shufflehi_epi16/0x93", &__v, sizeof(__v)); }
    { __m128i __v = _mm_shufflelo_epi16(i128[0], 0x3); show("_mm_shufflelo_epi16/0x3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_shufflelo_epi16(i128[0], 0x1b); show("_mm_shufflelo_epi16/0x1b", &__v, sizeof(__v)); }
    { __m128i __v = _mm_shufflelo_epi16(i128[0], 0x93); show("_mm_shufflelo_epi16/0x93", &__v, sizeof(__v)); }
    { __m128i __v = _mm_slli_si128(i128[0], 0x3); show("_mm_slli_si128/0x3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_slli_si128(i128[0], 0x1b); show("_mm_slli_si128/0x1b", &__v, sizeof(__v)); }
    { __m128i __v = _mm_slli_si128(i128[0], 0x93); show("_mm_slli_si128/0x93", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srli_si128(i128[0], 0x3); show("_mm_srli_si128/0x3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srli_si128(i128[0], 0x1b); show("_mm_srli_si128/0x1b", &__v, sizeof(__v)); }
    { __m128i __v = _mm_srli_si128(i128[0], 0x93); show("_mm_srli_si128/0x93", &__v, sizeof(__v)); }
    { __m128i __v = _mm_alignr_epi8(i128[0], i128[1], 0x3); show("_mm_alignr_epi8/0x3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_alignr_epi8(i128[0], i128[1], 0x1b); show("_mm_alignr_epi8/0x1b", &__v, sizeof(__v)); }
    { __m128i __v = _mm_alignr_epi8(i128[0], i128[1], 0x93); show("_mm_alignr_epi8/0x93", &__v, sizeof(__v)); }
    { __m128i __v = _mm_blend_epi16(i128[0], i128[1], 0x3); show("_mm_blend_epi16/0x3", &__v, sizeof(__v)); }
    { __m128i __v = _mm_blend_epi16(i128[0], i128[1], 0x1b); show("_mm_blend_epi16/0x1b", &__v, sizeof(__v)); }
    { __m128i __v = _mm_blend_epi16(i128[0], i128[1], 0x93); show("_mm_blend_epi16/0x93", &__v, sizeof(__v)); }
    { __m128 __v = _mm_shuffle_ps(f128[0], f128[1], 0x3); show("_mm_shuffle_ps/0x3", &__v, sizeof(__v)); }
    { __m128 __v = _mm_shuffle_ps(f128[0], f128[1], 0x1b); show("_mm_shuffle_ps/0x1b", &__v, sizeof(__v)); }
    { __m128 __v = _mm_shuffle_ps(f128[0], f128[1], 0x93); show("_mm_shuffle_ps/0x93", &__v, sizeof(__v)); }
    { __m128 __v = _mm_blend_ps(f128[0], f128[1], 0x3); show("_mm_blend_ps/0x3", &__v, sizeof(__v)); }
    { __m128 __v = _mm_blend_ps(f128[0], f128[1], 0xb); show("_mm_blend_ps/0xb", &__v, sizeof(__v)); }
    { __m128 __v = _mm_round_ps(f128[0], 0x1); show("_mm_round_ps/0x1", &__v, sizeof(__v)); }
    { __m128 __v = _mm_round_ps(f128[0], 0x2); show("_mm_round_ps/0x2", &__v, sizeof(__v)); }
    { __m128 __v = _mm_round_ps(f128[0], 0x3); show("_mm_round_ps/0x3", &__v, sizeof(__v)); }
    { __m128 __v = _mm_round_ps(f128[0], 0x4); show("_mm_round_ps/0x4", &__v, sizeof(__v)); }
    { __m128d __v = _mm_shuffle_pd(d128[0], d128[1], 0x3); show("_mm_shuffle_pd/0x3", &__v, sizeof(__v)); }
    { __m128d __v = _mm_blend_pd(d128[0], d128[1], 0x3); show("_mm_blend_pd/0x3", &__v, sizeof(__v)); }
    { __m128d __v = _mm_round_pd(d128[0], 0x1); show("_mm_round_pd/0x1", &__v, sizeof(__v)); }
    { __m128d __v = _mm_round_pd(d128[0], 0x2); show("_mm_round_pd/0x2", &__v, sizeof(__v)); }
    { __m128d __v = _mm_round_pd(d128[0], 0x3); show("_mm_round_pd/0x3", &__v, sizeof(__v)); }
    { __m128d __v = _mm_round_pd(d128[0], 0x4); show("_mm_round_pd/0x4", &__v, sizeof(__v)); }
    { __m128 __v = _mm_dp_ps(f128[0], f128[1], 0x3); show("_mm_dp_ps/0x3", &__v, sizeof(__v)); }
    { __m128 __v = _mm_dp_ps(f128[0], f128[1], 0x1b); show("_mm_dp_ps/0x1b", &__v, sizeof(__v)); }
    { __m128 __v = _mm_dp_ps(f128[0], f128[1], 0x93); show("_mm_dp_ps/0x93", &__v, sizeof(__v)); }
    { __m128d __v = _mm_dp_pd(d128[0], d128[1], 0x3); show("_mm_dp_pd/0x3", &__v, sizeof(__v)); }
    { __m128d __v = _mm_dp_pd(d128[0], d128[1], 0x1b); show("_mm_dp_pd/0x1b", &__v, sizeof(__v)); }
    { __m128d __v = _mm_dp_pd(d128[0], d128[1], 0x93); show("_mm_dp_pd/0x93", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_bslli_epi128(i256[0], 3); show("_mm256_bslli_epi128", &__v, sizeof(__v)); }
    { __m256i __v = _mm256_bsrli_epi128(i256[0], 3); show("_mm256_bsrli_epi128", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_floor_ps(f256[0]); show("_mm256_floor_ps", &__v, sizeof(__v)); }
    { __m256 __v = _mm256_ceil_ps(f256[0]); show("_mm256_ceil_ps", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_floor_pd(d256[0]); show("_mm256_floor_pd", &__v, sizeof(__v)); }
    { __m256d __v = _mm256_ceil_pd(d256[0]); show("_mm256_ceil_pd", &__v, sizeof(__v)); }
    { __m128i __v = _mm_bslli_si128(i128[0], 3); show("_mm_bslli_si128", &__v, sizeof(__v)); }
    { __m128i __v = _mm_bsrli_si128(i128[0], 3); show("_mm_bsrli_si128", &__v, sizeof(__v)); }
    { __m128 __v = _mm_floor_ps(f128[0]); show("_mm_floor_ps", &__v, sizeof(__v)); }
    { __m128 __v = _mm_ceil_ps(f128[0]); show("_mm_ceil_ps", &__v, sizeof(__v)); }
    { __m128d __v = _mm_floor_pd(d128[0]); show("_mm_floor_pd", &__v, sizeof(__v)); }
    { __m128d __v = _mm_ceil_pd(d128[0]); show("_mm_ceil_pd", &__v, sizeof(__v)); }
}

static int simd_supported(void)
{
    unsigned a, b, c, d, lo, hi;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c >> 27 & 1) || !(c >> 28 & 1) || !(c >> 12 & 1))     /* OSXSAVE, AVX, FMA */
        return 0;
    __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6)
        return 0;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    return b >> 5 & 1;                                          /* AVX2 */
}

static void intrinsics(void)
{
    if (!simd_supported()) {
        printf("intrinsics: no AVX2 and FMA here\n");
        return;
    }
    simd_values();
    simd_calls();
    printf("intrinsics: %d calls\n", 1241);
}
#else
static void intrinsics(void)
{
    printf("intrinsics: x86-64's\n");
}
#endif
