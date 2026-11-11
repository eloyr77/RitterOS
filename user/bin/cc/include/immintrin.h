/*
 * <immintrin.h> for RitterOS's cc (32d): the SSE to SSE4.1, AVX, AVX2 and
 * FMA intrinsics, Intel's names and meanings. The vector types are GCC's
 * (vector_size: values of their own in xmm/ymm registers, passed and
 * returned there as clang does); each operation is a small function (a
 * macro where an argument must be an immediate) around its instruction as
 * inline assembly with "x" operands, which cc gives registers as it does
 * an instruction's (32h) and inlines. set, setr, extract and insert are C.
 * The 128-bit operations of SSE use SSE's encodings (every x86-64 has
 * them); the 256-bit ones and AVX2's 128-bit ones need AVX/AVX2 on the
 * processor (ask CPUID and XGETBV first). xmm0 and xmm7, where a template
 * names them, are its own (cc's scratch, never a value's).
 */
#ifndef _IMMINTRIN_H
#define _IMMINTRIN_H

#ifndef __x86_64__
#error "<immintrin.h> is x86-64's"
#endif

typedef float __m128 __attribute__((vector_size(16)));
typedef double __m128d __attribute__((vector_size(16)));
typedef long long __m128i __attribute__((vector_size(16)));
typedef float __m256 __attribute__((vector_size(32)));
typedef double __m256d __attribute__((vector_size(32)));
typedef long long __m256i __attribute__((vector_size(32)));

#define _MM_SHUFFLE(z, y, x, w) (((z) << 6) | ((y) << 4) | ((x) << 2) | (w))
#define _MM_FROUND_TO_NEAREST_INT 0x00
#define _MM_FROUND_TO_NEG_INF 0x01
#define _MM_FROUND_TO_POS_INF 0x02
#define _MM_FROUND_TO_ZERO 0x03
#define _MM_FROUND_CUR_DIRECTION 0x04
#define _MM_FROUND_RAISE_EXC 0x00
#define _MM_FROUND_NO_EXC 0x08
#define _MM_FROUND_NINT (_MM_FROUND_RAISE_EXC | _MM_FROUND_TO_NEAREST_INT)
#define _MM_FROUND_FLOOR (_MM_FROUND_RAISE_EXC | _MM_FROUND_TO_NEG_INF)
#define _MM_FROUND_CEIL (_MM_FROUND_RAISE_EXC | _MM_FROUND_TO_POS_INF)
#define _MM_FROUND_TRUNC (_MM_FROUND_RAISE_EXC | _MM_FROUND_TO_ZERO)
#define _MM_FROUND_RINT (_MM_FROUND_RAISE_EXC | _MM_FROUND_CUR_DIRECTION)
#define _MM_FROUND_NEARBYINT (_MM_FROUND_NO_EXC | _MM_FROUND_CUR_DIRECTION)

#define _CMP_EQ_OQ 0x00
#define _CMP_LT_OS 0x01
#define _CMP_LE_OS 0x02
#define _CMP_UNORD_Q 0x03
#define _CMP_NEQ_UQ 0x04
#define _CMP_NLT_US 0x05
#define _CMP_NLE_US 0x06
#define _CMP_ORD_Q 0x07
#define _CMP_EQ_UQ 0x08
#define _CMP_NGE_US 0x09
#define _CMP_NGT_US 0x0a
#define _CMP_FALSE_OQ 0x0b
#define _CMP_NEQ_OQ 0x0c
#define _CMP_GE_OS 0x0d
#define _CMP_GT_OS 0x0e
#define _CMP_TRUE_UQ 0x0f
#define _CMP_EQ_OS 0x10
#define _CMP_LT_OQ 0x11
#define _CMP_LE_OQ 0x12
#define _CMP_UNORD_S 0x13
#define _CMP_NEQ_US 0x14
#define _CMP_NLT_UQ 0x15
#define _CMP_NLE_UQ 0x16
#define _CMP_ORD_S 0x17
#define _CMP_EQ_US 0x18
#define _CMP_NGE_UQ 0x19
#define _CMP_NGT_UQ 0x1a
#define _CMP_FALSE_OS 0x1b
#define _CMP_NEQ_OS 0x1c
#define _CMP_GE_OQ 0x1d
#define _CMP_GT_OQ 0x1e
#define _CMP_TRUE_US 0x1f

/* the elements of a vector, for the C parts */
union __ritteros_v128 { __m128i __i; __m128 __f; __m128d __d; signed char __b[16]; short __w[8]; int __l[4];
                        long long __q[2]; float __s[4]; double __e[2]; };
union __ritteros_v256 { __m256i __i; __m256 __f; __m256d __d; signed char __b[32]; short __w[16]; int __l[8];
                        long long __q[4]; float __s[8]; double __e[4]; __m128i __h[2]; __m128 __hf[2];
                        __m128d __hd[2]; };
#define __RI static __inline__

/* --- 256 bits (AVX, AVX2) --- */

__RI __m256i _mm256_setzero_si256(void) { __m256i __r; __asm__("vxorps %x0, %x0, %x0" : "=x"(__r) : ); return __r; }
__RI __m256 _mm256_setzero_ps(void) { __m256 __r; __asm__("vxorps %x0, %x0, %x0" : "=x"(__r) : ); return __r; }
__RI __m256d _mm256_setzero_pd(void) { __m256d __r; __asm__("vxorps %x0, %x0, %x0" : "=x"(__r) : ); return __r; }
__RI __m256i _mm256_undefined_si256(void) { return _mm256_setzero_si256(); }
__RI __m256 _mm256_undefined_ps(void) { return _mm256_setzero_ps(); }
__RI __m256d _mm256_undefined_pd(void) { return _mm256_setzero_pd(); }
__RI __m256i _mm256_set1_epi8(char __x) { union __ritteros_v256 __u; for (int __k = 0; __k < 32; __k++) __u.__b[__k] = __x; return __u.__i; }
__RI __m256i _mm256_set_epi8(char __e31, char __e30, char __e29, char __e28, char __e27, char __e26, char __e25, char __e24, char __e23, char __e22, char __e21, char __e20, char __e19, char __e18, char __e17, char __e16, char __e15, char __e14, char __e13, char __e12, char __e11, char __e10, char __e9, char __e8, char __e7, char __e6, char __e5, char __e4, char __e3, char __e2, char __e1, char __e0) { union __ritteros_v256 __u = { .__b = { __e0, __e1, __e2, __e3, __e4, __e5, __e6, __e7, __e8, __e9, __e10, __e11, __e12, __e13, __e14, __e15, __e16, __e17, __e18, __e19, __e20, __e21, __e22, __e23, __e24, __e25, __e26, __e27, __e28, __e29, __e30, __e31 } }; return __u.__i; }
__RI __m256i _mm256_setr_epi8(char __e0, char __e1, char __e2, char __e3, char __e4, char __e5, char __e6, char __e7, char __e8, char __e9, char __e10, char __e11, char __e12, char __e13, char __e14, char __e15, char __e16, char __e17, char __e18, char __e19, char __e20, char __e21, char __e22, char __e23, char __e24, char __e25, char __e26, char __e27, char __e28, char __e29, char __e30, char __e31) { union __ritteros_v256 __u = { .__b = { __e0, __e1, __e2, __e3, __e4, __e5, __e6, __e7, __e8, __e9, __e10, __e11, __e12, __e13, __e14, __e15, __e16, __e17, __e18, __e19, __e20, __e21, __e22, __e23, __e24, __e25, __e26, __e27, __e28, __e29, __e30, __e31 } }; return __u.__i; }
__RI __m256i _mm256_set1_epi16(short __x) { union __ritteros_v256 __u; for (int __k = 0; __k < 16; __k++) __u.__w[__k] = __x; return __u.__i; }
__RI __m256i _mm256_set_epi16(short __e15, short __e14, short __e13, short __e12, short __e11, short __e10, short __e9, short __e8, short __e7, short __e6, short __e5, short __e4, short __e3, short __e2, short __e1, short __e0) { union __ritteros_v256 __u = { .__w = { __e0, __e1, __e2, __e3, __e4, __e5, __e6, __e7, __e8, __e9, __e10, __e11, __e12, __e13, __e14, __e15 } }; return __u.__i; }
__RI __m256i _mm256_setr_epi16(short __e0, short __e1, short __e2, short __e3, short __e4, short __e5, short __e6, short __e7, short __e8, short __e9, short __e10, short __e11, short __e12, short __e13, short __e14, short __e15) { union __ritteros_v256 __u = { .__w = { __e0, __e1, __e2, __e3, __e4, __e5, __e6, __e7, __e8, __e9, __e10, __e11, __e12, __e13, __e14, __e15 } }; return __u.__i; }
__RI __m256i _mm256_set1_epi32(int __x) { __m256i __r; __asm__("vmovd %1, %x0\n\tvpshufd $0, %x0, %x0\n\tvinsertf128 $1, %x0, %0, %0" : "=x"(__r) : "r"(__x)); return __r; }
__RI __m256i _mm256_set_epi32(int __e7, int __e6, int __e5, int __e4, int __e3, int __e2, int __e1, int __e0) { union __ritteros_v256 __u = { .__l = { __e0, __e1, __e2, __e3, __e4, __e5, __e6, __e7 } }; return __u.__i; }
__RI __m256i _mm256_setr_epi32(int __e0, int __e1, int __e2, int __e3, int __e4, int __e5, int __e6, int __e7) { union __ritteros_v256 __u = { .__l = { __e0, __e1, __e2, __e3, __e4, __e5, __e6, __e7 } }; return __u.__i; }
__RI __m256i _mm256_set1_epi64x(long long __x) { union __ritteros_v256 __u; for (int __k = 0; __k < 4; __k++) __u.__q[__k] = __x; return __u.__i; }
__RI __m256i _mm256_set_epi64x(long long __e3, long long __e2, long long __e1, long long __e0) { union __ritteros_v256 __u = { .__q = { __e0, __e1, __e2, __e3 } }; return __u.__i; }
__RI __m256i _mm256_setr_epi64x(long long __e0, long long __e1, long long __e2, long long __e3) { union __ritteros_v256 __u = { .__q = { __e0, __e1, __e2, __e3 } }; return __u.__i; }
__RI __m256 _mm256_set1_ps(float __x) { __m256 __r; __asm__("vshufps $0, %x1, %x1, %x0\n\tvinsertf128 $1, %x0, %0, %0" : "=x"(__r) : "x"(__x)); return __r; }
__RI __m256 _mm256_set_ps(float __e7, float __e6, float __e5, float __e4, float __e3, float __e2, float __e1, float __e0) { union __ritteros_v256 __u = { .__s = { __e0, __e1, __e2, __e3, __e4, __e5, __e6, __e7 } }; return __u.__f; }
__RI __m256 _mm256_setr_ps(float __e0, float __e1, float __e2, float __e3, float __e4, float __e5, float __e6, float __e7) { union __ritteros_v256 __u = { .__s = { __e0, __e1, __e2, __e3, __e4, __e5, __e6, __e7 } }; return __u.__f; }
__RI __m256d _mm256_set1_pd(double __x) { __m256d __r; __asm__("vmovddup %x1, %x0\n\tvinsertf128 $1, %x0, %0, %0" : "=x"(__r) : "x"(__x)); return __r; }
__RI __m256d _mm256_set_pd(double __e3, double __e2, double __e1, double __e0) { union __ritteros_v256 __u = { .__e = { __e0, __e1, __e2, __e3 } }; return __u.__d; }
__RI __m256d _mm256_setr_pd(double __e0, double __e1, double __e2, double __e3) { union __ritteros_v256 __u = { .__e = { __e0, __e1, __e2, __e3 } }; return __u.__d; }
__RI __m256i _mm256_set_m128i(__m128i __hi, __m128i __lo) { union __ritteros_v256 __u = { .__h = { __lo, __hi } }; return __u.__i; }
__RI __m256i _mm256_setr_m128i(__m128i __lo, __m128i __hi) { return _mm256_set_m128i(__hi, __lo); }
__RI __m256 _mm256_set_m128(__m128 __hi, __m128 __lo) { union __ritteros_v256 __u = { .__hf = { __lo, __hi } }; return __u.__f; }
__RI __m256d _mm256_set_m128d(__m128d __hi, __m128d __lo) { union __ritteros_v256 __u = { .__hd = { __lo, __hi } }; return __u.__d; }
__RI __m256i _mm256_load_si256(const __m256i *__p) { return *(const __m256i *)__p; }
__RI __m256i _mm256_loadu_si256(const __m256i *__p) { return *(const __m256i *)__p; }
__RI void _mm256_store_si256(__m256i *__p, __m256i __a) { *(__m256i *)__p = __a; }
__RI void _mm256_storeu_si256(__m256i *__p, __m256i __a) { *(__m256i *)__p = __a; }
__RI void _mm256_stream_si256(__m256i *__p, __m256i __a) { *(__m256i *)__p = __a; }
__RI __m256 _mm256_load_ps(const float *__p) { return *(const __m256 *)__p; }
__RI __m256 _mm256_loadu_ps(const float *__p) { return *(const __m256 *)__p; }
__RI void _mm256_store_ps(float *__p, __m256 __a) { *(__m256 *)__p = __a; }
__RI void _mm256_storeu_ps(float *__p, __m256 __a) { *(__m256 *)__p = __a; }
__RI void _mm256_stream_ps(float *__p, __m256 __a) { *(__m256 *)__p = __a; }
__RI __m256d _mm256_load_pd(const double *__p) { return *(const __m256d *)__p; }
__RI __m256d _mm256_loadu_pd(const double *__p) { return *(const __m256d *)__p; }
__RI void _mm256_store_pd(double *__p, __m256d __a) { *(__m256d *)__p = __a; }
__RI void _mm256_storeu_pd(double *__p, __m256d __a) { *(__m256d *)__p = __a; }
__RI void _mm256_stream_pd(double *__p, __m256d __a) { *(__m256d *)__p = __a; }
__RI __m256i _mm256_lddqu_si256(const __m256i *__p) { return *__p; }
__RI __m256i _mm256_stream_load_si256(const __m256i *__p) { return *__p; }
__RI __m256i _mm256_maskload_epi32(const int *__p, __m256i __m) { __m256i __r; __asm__("vmovdqu %2, %%ymm1\n\tvpmaskmovd %1, %%ymm1, %%ymm0\n\tvmovdqu %%ymm0, %0" : "=m"(__r) : "m"(*(const __m256i *)__p), "m"(__m) : "xmm0", "xmm1"); return __r; }
__RI void _mm256_maskstore_epi32(int *__p, __m256i __m, __m256i __a) { __asm__("vmovdqu %1, %%ymm1\n\tvmovdqu %2, %%ymm0\n\tvpmaskmovd %%ymm0, %%ymm1, %0" : "+m"(*(__m256i *)__p) : "m"(__m), "m"(__a) : "xmm0", "xmm1"); }
__RI __m128i _mm_maskload_epi32(const int *__p, __m128i __m) { __m128i __r; __asm__("vmovdqu %2, %%xmm1\n\tvpmaskmovd %1, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, %0" : "=m"(__r) : "m"(*(const __m128i *)__p), "m"(__m) : "xmm0", "xmm1"); return __r; }
__RI void _mm_maskstore_epi32(int *__p, __m128i __m, __m128i __a) { __asm__("vmovdqu %1, %%xmm1\n\tvmovdqu %2, %%xmm0\n\tvpmaskmovd %%xmm0, %%xmm1, %0" : "+m"(*(__m128i *)__p) : "m"(__m), "m"(__a) : "xmm0", "xmm1"); }
__RI __m256i _mm256_maskload_epi64(const long long *__p, __m256i __m) { __m256i __r; __asm__("vmovdqu %2, %%ymm1\n\tvpmaskmovq %1, %%ymm1, %%ymm0\n\tvmovdqu %%ymm0, %0" : "=m"(__r) : "m"(*(const __m256i *)__p), "m"(__m) : "xmm0", "xmm1"); return __r; }
__RI void _mm256_maskstore_epi64(long long *__p, __m256i __m, __m256i __a) { __asm__("vmovdqu %1, %%ymm1\n\tvmovdqu %2, %%ymm0\n\tvpmaskmovq %%ymm0, %%ymm1, %0" : "+m"(*(__m256i *)__p) : "m"(__m), "m"(__a) : "xmm0", "xmm1"); }
__RI __m128i _mm_maskload_epi64(const long long *__p, __m128i __m) { __m128i __r; __asm__("vmovdqu %2, %%xmm1\n\tvpmaskmovq %1, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, %0" : "=m"(__r) : "m"(*(const __m128i *)__p), "m"(__m) : "xmm0", "xmm1"); return __r; }
__RI void _mm_maskstore_epi64(long long *__p, __m128i __m, __m128i __a) { __asm__("vmovdqu %1, %%xmm1\n\tvmovdqu %2, %%xmm0\n\tvpmaskmovq %%xmm0, %%xmm1, %0" : "+m"(*(__m128i *)__p) : "m"(__m), "m"(__a) : "xmm0", "xmm1"); }
__RI __m256 _mm256_maskload_ps(const float *__p, __m256i __m) { __m256 __r; __asm__("vmovdqu %2, %%ymm1\n\tvmaskmovps %1, %%ymm1, %%ymm0\n\tvmovdqu %%ymm0, %0" : "=m"(__r) : "m"(*(const __m256 *)__p), "m"(__m) : "xmm0", "xmm1"); return __r; }
__RI void _mm256_maskstore_ps(float *__p, __m256i __m, __m256 __a) { __asm__("vmovdqu %1, %%ymm1\n\tvmovdqu %2, %%ymm0\n\tvmaskmovps %%ymm0, %%ymm1, %0" : "+m"(*(__m256 *)__p) : "m"(__m), "m"(__a) : "xmm0", "xmm1"); }
__RI __m128 _mm_maskload_ps(const float *__p, __m128i __m) { __m128 __r; __asm__("vmovdqu %2, %%xmm1\n\tvmaskmovps %1, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, %0" : "=m"(__r) : "m"(*(const __m128 *)__p), "m"(__m) : "xmm0", "xmm1"); return __r; }
__RI void _mm_maskstore_ps(float *__p, __m128i __m, __m128 __a) { __asm__("vmovdqu %1, %%xmm1\n\tvmovdqu %2, %%xmm0\n\tvmaskmovps %%xmm0, %%xmm1, %0" : "+m"(*(__m128 *)__p) : "m"(__m), "m"(__a) : "xmm0", "xmm1"); }
__RI __m256d _mm256_maskload_pd(const double *__p, __m256i __m) { __m256d __r; __asm__("vmovdqu %2, %%ymm1\n\tvmaskmovpd %1, %%ymm1, %%ymm0\n\tvmovdqu %%ymm0, %0" : "=m"(__r) : "m"(*(const __m256d *)__p), "m"(__m) : "xmm0", "xmm1"); return __r; }
__RI void _mm256_maskstore_pd(double *__p, __m256i __m, __m256d __a) { __asm__("vmovdqu %1, %%ymm1\n\tvmovdqu %2, %%ymm0\n\tvmaskmovpd %%ymm0, %%ymm1, %0" : "+m"(*(__m256d *)__p) : "m"(__m), "m"(__a) : "xmm0", "xmm1"); }
__RI __m128d _mm_maskload_pd(const double *__p, __m128i __m) { __m128d __r; __asm__("vmovdqu %2, %%xmm1\n\tvmaskmovpd %1, %%xmm1, %%xmm0\n\tvmovdqu %%xmm0, %0" : "=m"(__r) : "m"(*(const __m128d *)__p), "m"(__m) : "xmm0", "xmm1"); return __r; }
__RI void _mm_maskstore_pd(double *__p, __m128i __m, __m128d __a) { __asm__("vmovdqu %1, %%xmm1\n\tvmovdqu %2, %%xmm0\n\tvmaskmovpd %%xmm0, %%xmm1, %0" : "+m"(*(__m128d *)__p) : "m"(__m), "m"(__a) : "xmm0", "xmm1"); }
__RI __m128i _mm256_castsi256_si128(__m256i __a) { __m128i __r; __asm__("vmovaps %x1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_castsi128_si256(__m128i __a) { __m256i __r; __asm__("vmovaps %1, %x0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_zextsi128_si256(__m128i __a) { return _mm256_castsi128_si256(__a); }
__RI __m128 _mm256_castps256_ps128(__m256 __a) { __m128 __r; __asm__("vmovaps %x1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256 _mm256_castps128_ps256(__m128 __a) { __m256 __r; __asm__("vmovaps %1, %x0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256 _mm256_zextps128_ps256(__m128 __a) { return _mm256_castps128_ps256(__a); }
__RI __m128d _mm256_castpd256_pd128(__m256d __a) { __m128d __r; __asm__("vmovaps %x1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256d _mm256_castpd128_pd256(__m128d __a) { __m256d __r; __asm__("vmovaps %1, %x0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256d _mm256_zextpd128_pd256(__m128d __a) { return _mm256_castpd128_pd256(__a); }
__RI __m256i _mm256_castps_si256(__m256 __a) { __m256i __r; __asm__("vmovaps %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256 _mm256_castsi256_ps(__m256i __a) { __m256 __r; __asm__("vmovaps %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_castpd_si256(__m256d __a) { __m256i __r; __asm__("vmovaps %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256d _mm256_castsi256_pd(__m256i __a) { __m256d __r; __asm__("vmovaps %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256d _mm256_castps_pd(__m256 __a) { __m256d __r; __asm__("vmovaps %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256 _mm256_castpd_ps(__m256d __a) { __m256 __r; __asm__("vmovaps %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI int _mm256_cvtsi256_si32(__m256i __a) { int __r; __asm__("vmovd %x1, %0" : "=r"(__r) : "x"(__a)); return __r; }
__RI float _mm256_cvtss_f32(__m256 __a) { float __r; __asm__("vmovaps %x1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI double _mm256_cvtsd_f64(__m256d __a) { double __r; __asm__("vmovaps %x1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm256_extracti128_si256(__m256i __a, const int __k) { union __ritteros_v256 __u = { __a }; return __u.__h[__k & 1]; }
__RI __m128i _mm256_extractf128_si256(__m256i __a, const int __k) { return _mm256_extracti128_si256(__a, __k); }
__RI __m128 _mm256_extractf128_ps(__m256 __a, const int __k) { union __ritteros_v256 __u; __u.__f = __a; return __u.__hf[__k & 1]; }
__RI __m128d _mm256_extractf128_pd(__m256d __a, const int __k) { union __ritteros_v256 __u; __u.__d = __a; return __u.__hd[__k & 1]; }
__RI __m256i _mm256_inserti128_si256(__m256i __a, __m128i __b, const int __k) { union __ritteros_v256 __u = { __a }; __u.__h[__k & 1] = __b; return __u.__i; }
__RI __m256i _mm256_insertf128_si256(__m256i __a, __m128i __b, const int __k) { return _mm256_inserti128_si256(__a, __b, __k); }
__RI __m256 _mm256_insertf128_ps(__m256 __a, __m128 __b, const int __k) { union __ritteros_v256 __u; __u.__f = __a; __u.__hf[__k & 1] = __b; return __u.__f; }
__RI __m256d _mm256_insertf128_pd(__m256d __a, __m128d __b, const int __k) { union __ritteros_v256 __u; __u.__d = __a; __u.__hd[__k & 1] = __b; return __u.__d; }
__RI int _mm256_extract_epi8(__m256i __a, const int __k) { union __ritteros_v256 __u = { __a }; return (unsigned char)__u.__b[__k & 31]; }
__RI int _mm256_extract_epi16(__m256i __a, const int __k) { union __ritteros_v256 __u = { __a }; return (unsigned short)__u.__w[__k & 15]; }
__RI int _mm256_extract_epi32(__m256i __a, const int __k) { union __ritteros_v256 __u = { __a }; return __u.__l[__k & 7]; }
__RI long long _mm256_extract_epi64(__m256i __a, const int __k) { union __ritteros_v256 __u = { __a }; return __u.__q[__k & 3]; }
__RI __m256i _mm256_insert_epi8(__m256i __a, int __x, const int __k) { union __ritteros_v256 __u = { __a }; __u.__b[__k & 31] = (signed char)__x; return __u.__i; }
__RI __m256i _mm256_insert_epi16(__m256i __a, int __x, const int __k) { union __ritteros_v256 __u = { __a }; __u.__w[__k & 15] = (short)__x; return __u.__i; }
__RI __m256i _mm256_insert_epi32(__m256i __a, int __x, const int __k) { union __ritteros_v256 __u = { __a }; __u.__l[__k & 7] = __x; return __u.__i; }
__RI __m256i _mm256_insert_epi64(__m256i __a, long long __x, const int __k) { union __ritteros_v256 __u = { __a }; __u.__q[__k & 3] = __x; return __u.__i; }
__RI __m256 _mm256_broadcast_ss(const float *__p) { return _mm256_set1_ps(*__p); }
__RI __m256d _mm256_broadcast_sd(const double *__p) { return _mm256_set1_pd(*__p); }
__RI __m256 _mm256_broadcast_ps(const __m128 *__p) { union __ritteros_v256 __u = { .__hf = { *__p, *__p } }; return __u.__f; }
__RI __m256d _mm256_broadcast_pd(const __m128d *__p) { union __ritteros_v256 __u = { .__hd = { *__p, *__p } }; return __u.__d; }
__RI __m256i _mm256_broadcastsi128_si256(__m128i __a) { __m256i __r; __asm__("vinsertf128 $1, %1, %t1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm_broadcastsi128_si256(__m128i __a) { return _mm256_broadcastsi128_si256(__a); }
__RI __m256i _mm256_broadcastb_epi8(__m128i __a) { __m256i __r; __asm__("vpbroadcastb %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_broadcastw_epi16(__m128i __a) { __m256i __r; __asm__("vpbroadcastw %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_broadcastd_epi32(__m128i __a) { __m256i __r; __asm__("vpbroadcastd %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_broadcastq_epi64(__m128i __a) { __m256i __r; __asm__("vpbroadcastq %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256 _mm256_broadcastss_ps(__m128 __a) { __m256 __r; __asm__("vbroadcastss %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256d _mm256_broadcastsd_pd(__m128d __a) { __m256d __r; __asm__("vbroadcastsd %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI void _mm256_zeroupper(void) { __asm__ __volatile__("vzeroupper" : : : "memory"); }
__RI void _mm256_zeroall(void) { __asm__ __volatile__("vzeroall" : : : "memory"); }
__RI __m256i _mm256_add_epi8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpaddb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_add_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpaddw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_add_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpaddd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_add_epi64(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpaddq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_sub_epi8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsubb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_sub_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsubw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_sub_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsubd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_sub_epi64(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsubq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_adds_epi8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpaddsb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_adds_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpaddsw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_adds_epu8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpaddusb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_adds_epu16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpaddusw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_subs_epi8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsubsb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_subs_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsubsw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_subs_epu8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsubusb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_subs_epu16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsubusw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_mullo_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmullw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_mullo_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmulld %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_mulhi_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmulhw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_mulhi_epu16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmulhuw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_mul_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmuldq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_mul_epu32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmuludq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_madd_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmaddwd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_maddubs_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmaddubsw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_sad_epu8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsadbw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_avg_epu8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpavgb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_avg_epu16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpavgw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_mulhrs_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmulhrsw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_min_epi8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpminsb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_min_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpminsw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_min_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpminsd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_min_epu8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpminub %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_min_epu16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpminuw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_min_epu32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpminud %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_max_epi8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmaxsb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_max_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmaxsw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_max_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmaxsd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_max_epu8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmaxub %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_max_epu16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmaxuw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_max_epu32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpmaxud %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_sign_epi8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsignb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_sign_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsignw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_sign_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsignd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_hadd_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vphaddw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_hadd_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vphaddd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_hadds_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vphaddsw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_hsub_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vphsubw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_hsub_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vphsubd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_hsubs_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vphsubsw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_and_si256(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpand %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_or_si256(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpor %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_xor_si256(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpxor %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_andnot_si256(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpandn %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_cmpeq_epi8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpcmpeqb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_cmpeq_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpcmpeqw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_cmpeq_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpcmpeqd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_cmpeq_epi64(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpcmpeqq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_cmpgt_epi8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpcmpgtb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_cmpgt_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpcmpgtw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_cmpgt_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpcmpgtd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_cmpgt_epi64(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpcmpgtq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_shuffle_epi8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpshufb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_unpacklo_epi8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpunpcklbw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_unpacklo_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpunpcklwd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_unpacklo_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpunpckldq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_unpacklo_epi64(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpunpcklqdq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_unpackhi_epi8(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpunpckhbw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_unpackhi_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpunpckhwd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_unpackhi_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpunpckhdq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_unpackhi_epi64(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpunpckhqdq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_packs_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpacksswb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_packs_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpackssdw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_packus_epi16(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpackuswb %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_packus_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpackusdw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_sllv_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsllvd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_sllv_epi64(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsllvq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_srlv_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsrlvd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_srlv_epi64(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsrlvq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_srav_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpsravd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_permutevar8x32_epi32(__m256i __a, __m256i __b) { __m256i __r; __asm__("vpermd %1, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_sllv_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("vpsllvd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_sllv_epi64(__m128i __a, __m128i __b) { __m128i __r; __asm__("vpsllvq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_srlv_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("vpsrlvd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_srlv_epi64(__m128i __a, __m128i __b) { __m128i __r; __asm__("vpsrlvq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_srav_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("vpsravd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256i _mm256_slli_epi16(__m256i __a, int __n) { __m256i __r; int __c = __n & 255; __asm__("vmovd %k2, %x0\n\tvpsllw %x0, %1, %0" : "=x"(__r) : "x"(__a), "r"(__c)); return __r; }
__RI __m256i _mm256_slli_epi32(__m256i __a, int __n) { __m256i __r; int __c = __n & 255; __asm__("vmovd %k2, %x0\n\tvpslld %x0, %1, %0" : "=x"(__r) : "x"(__a), "r"(__c)); return __r; }
__RI __m256i _mm256_slli_epi64(__m256i __a, int __n) { __m256i __r; int __c = __n & 255; __asm__("vmovd %k2, %x0\n\tvpsllq %x0, %1, %0" : "=x"(__r) : "x"(__a), "r"(__c)); return __r; }
__RI __m256i _mm256_srli_epi16(__m256i __a, int __n) { __m256i __r; int __c = __n & 255; __asm__("vmovd %k2, %x0\n\tvpsrlw %x0, %1, %0" : "=x"(__r) : "x"(__a), "r"(__c)); return __r; }
__RI __m256i _mm256_srli_epi32(__m256i __a, int __n) { __m256i __r; int __c = __n & 255; __asm__("vmovd %k2, %x0\n\tvpsrld %x0, %1, %0" : "=x"(__r) : "x"(__a), "r"(__c)); return __r; }
__RI __m256i _mm256_srli_epi64(__m256i __a, int __n) { __m256i __r; int __c = __n & 255; __asm__("vmovd %k2, %x0\n\tvpsrlq %x0, %1, %0" : "=x"(__r) : "x"(__a), "r"(__c)); return __r; }
__RI __m256i _mm256_srai_epi16(__m256i __a, int __n) { __m256i __r; int __c = __n & 255; __asm__("vmovd %k2, %x0\n\tvpsraw %x0, %1, %0" : "=x"(__r) : "x"(__a), "r"(__c)); return __r; }
__RI __m256i _mm256_srai_epi32(__m256i __a, int __n) { __m256i __r; int __c = __n & 255; __asm__("vmovd %k2, %x0\n\tvpsrad %x0, %1, %0" : "=x"(__r) : "x"(__a), "r"(__c)); return __r; }
__RI __m256i _mm256_sll_epi16(__m256i __a, __m128i __n) { __m256i __r; __asm__("vpsllw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__n)); return __r; }
__RI __m256i _mm256_sll_epi32(__m256i __a, __m128i __n) { __m256i __r; __asm__("vpslld %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__n)); return __r; }
__RI __m256i _mm256_sll_epi64(__m256i __a, __m128i __n) { __m256i __r; __asm__("vpsllq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__n)); return __r; }
__RI __m256i _mm256_srl_epi16(__m256i __a, __m128i __n) { __m256i __r; __asm__("vpsrlw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__n)); return __r; }
__RI __m256i _mm256_srl_epi32(__m256i __a, __m128i __n) { __m256i __r; __asm__("vpsrld %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__n)); return __r; }
__RI __m256i _mm256_srl_epi64(__m256i __a, __m128i __n) { __m256i __r; __asm__("vpsrlq %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__n)); return __r; }
__RI __m256i _mm256_sra_epi16(__m256i __a, __m128i __n) { __m256i __r; __asm__("vpsraw %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__n)); return __r; }
__RI __m256i _mm256_sra_epi32(__m256i __a, __m128i __n) { __m256i __r; __asm__("vpsrad %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__n)); return __r; }
__RI __m256i _mm256_abs_epi8(__m256i __a) { __m256i __r; __asm__("vpabsb %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_abs_epi16(__m256i __a) { __m256i __r; __asm__("vpabsw %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_abs_epi32(__m256i __a) { __m256i __r; __asm__("vpabsd %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_cvtepi8_epi16(__m128i __a) { __m256i __r; __asm__("vpmovsxbw %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_cvtepi8_epi32(__m128i __a) { __m256i __r; __asm__("vpmovsxbd %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_cvtepi8_epi64(__m128i __a) { __m256i __r; __asm__("vpmovsxbq %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_cvtepi16_epi32(__m128i __a) { __m256i __r; __asm__("vpmovsxwd %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_cvtepi16_epi64(__m128i __a) { __m256i __r; __asm__("vpmovsxwq %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_cvtepi32_epi64(__m128i __a) { __m256i __r; __asm__("vpmovsxdq %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_cvtepu8_epi16(__m128i __a) { __m256i __r; __asm__("vpmovzxbw %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_cvtepu8_epi32(__m128i __a) { __m256i __r; __asm__("vpmovzxbd %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_cvtepu8_epi64(__m128i __a) { __m256i __r; __asm__("vpmovzxbq %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_cvtepu16_epi32(__m128i __a) { __m256i __r; __asm__("vpmovzxwd %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_cvtepu16_epi64(__m128i __a) { __m256i __r; __asm__("vpmovzxwq %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_cvtepu32_epi64(__m128i __a) { __m256i __r; __asm__("vpmovzxdq %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI int _mm256_movemask_epi8(__m256i __a) { int __r; __asm__("vpmovmskb %1, %0" : "=r"(__r) : "x"(__a)); return __r; }
__RI int _mm256_movemask_ps(__m256 __a) { int __r; __asm__("vmovmskps %1, %0" : "=r"(__r) : "x"(__a)); return __r; }
__RI int _mm256_movemask_pd(__m256d __a) { int __r; __asm__("vmovmskpd %1, %0" : "=r"(__r) : "x"(__a)); return __r; }
__RI int _mm256_testz_si256(__m256i __a, __m256i __b) { int __r; __asm__("vptest %2, %1\n\tsete %b0\n\tmovzbl %b0, %k0" : "=r"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI int _mm256_testc_si256(__m256i __a, __m256i __b) { int __r; __asm__("vptest %2, %1\n\tsetb %b0\n\tmovzbl %b0, %k0" : "=r"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI int _mm256_testnzc_si256(__m256i __a, __m256i __b) { int __r; __asm__("vptest %2, %1\n\tseta %b0\n\tmovzbl %b0, %k0" : "=r"(__r) : "x"(__a), "x"(__b)); return __r; }
#define _mm256_shuffle_epi32(a, imm) __extension__({ __m256i __a = (a); __m256i __r; __asm__("vpshufd %2, %1, %0" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm256_shufflehi_epi16(a, imm) __extension__({ __m256i __a = (a); __m256i __r; __asm__("vpshufhw %2, %1, %0" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm256_shufflelo_epi16(a, imm) __extension__({ __m256i __a = (a); __m256i __r; __asm__("vpshuflw %2, %1, %0" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm256_permute4x64_epi64(a, imm) __extension__({ __m256i __a = (a); __m256i __r; __asm__("vpermq %2, %1, %0" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm256_slli_si256(a, imm) __extension__({ __m256i __a = (a); __m256i __r; __asm__("vpslldq %2, %1, %0" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm256_srli_si256(a, imm) __extension__({ __m256i __a = (a); __m256i __r; __asm__("vpsrldq %2, %1, %0" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm256_bslli_epi128(a, imm) _mm256_slli_si256(a, imm)
#define _mm256_bsrli_epi128(a, imm) _mm256_srli_si256(a, imm)
#define _mm256_alignr_epi8(a, b, imm) __extension__({ __m256i __a = (a), __b = (b), __r; __asm__("vpalignr %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm256_blend_epi16(a, b, imm) __extension__({ __m256i __a = (a), __b = (b), __r; __asm__("vpblendw %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm256_blend_epi32(a, b, imm) __extension__({ __m256i __a = (a), __b = (b), __r; __asm__("vpblendd %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm256_permute2x128_si256(a, b, imm) __extension__({ __m256i __a = (a), __b = (b), __r; __asm__("vperm2i128 %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm256_permute2f128_si256(a, b, imm) __extension__({ __m256i __a = (a), __b = (b), __r; __asm__("vperm2f128 %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm256_mpsadbw_epu8(a, b, imm) __extension__({ __m256i __a = (a), __b = (b), __r; __asm__("vmpsadbw %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm_blend_epi32(a, b, imm) __extension__({ __m128i __a = (a), __b = (b), __r; __asm__("vpblendd %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
__RI __m256i _mm256_blendv_epi8(__m256i __a, __m256i __b, __m256i __m) { __m256i __r; __asm__("vpblendvb %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__m)); return __r; }
__RI __m256 _mm256_add_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vaddps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_sub_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vsubps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_mul_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vmulps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_div_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vdivps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_min_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vminps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_max_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vmaxps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_and_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vandps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_or_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vorps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_xor_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vxorps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_andnot_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vandnps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_unpacklo_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vunpcklps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_unpackhi_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vunpckhps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_hadd_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vhaddps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_hsub_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vhsubps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_addsub_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vaddsubps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_sqrt_ps(__m256 __a) { __m256 __r; __asm__("vsqrtps %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256 _mm256_permutevar_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vpermilps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
#define _mm256_permute_ps(a, imm) __extension__({ __m256 __a = (a); __m256 __r; __asm__("vpermilps %2, %1, %0" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm256_round_ps(a, imm) __extension__({ __m256 __a = (a); __m256 __r; __asm__("vroundps %2, %1, %0" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm256_floor_ps(a) _mm256_round_ps(a, _MM_FROUND_FLOOR)
#define _mm256_ceil_ps(a) _mm256_round_ps(a, _MM_FROUND_CEIL)
#define _mm256_shuffle_ps(a, b, imm) __extension__({ __m256 __a = (a), __b = (b), __r; __asm__("vshufps %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm256_blend_ps(a, b, imm) __extension__({ __m256 __a = (a), __b = (b), __r; __asm__("vblendps %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm256_cmp_ps(a, b, imm) __extension__({ __m256 __a = (a), __b = (b), __r; __asm__("vcmpps %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm256_permute2f128_ps(a, b, imm) __extension__({ __m256 __a = (a), __b = (b), __r; __asm__("vperm2f128 %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm_cmp_ps(a, b, imm) __extension__({ __m128 __a = (a), __b = (b), __r; __asm__("vcmpps %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm_permute_ps(a, imm) __extension__({ __m128 __a = (a); __m128 __r; __asm__("vpermilps %2, %1, %0" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
__RI __m128 _mm_permutevar_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("vpermilps %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_blendv_ps(__m256 __a, __m256 __b, __m256 __m) { __m256 __r; __asm__("vblendvps %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__m)); return __r; }
__RI int _mm256_testz_ps(__m256 __a, __m256 __b) { int __r; __asm__("vtestps %2, %1\n\tsete %b0\n\tmovzbl %b0, %k0" : "=r"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI int _mm256_testc_ps(__m256 __a, __m256 __b) { int __r; __asm__("vtestps %2, %1\n\tsetb %b0\n\tmovzbl %b0, %k0" : "=r"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI int _mm256_testnzc_ps(__m256 __a, __m256 __b) { int __r; __asm__("vtestps %2, %1\n\tseta %b0\n\tmovzbl %b0, %k0" : "=r"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256 _mm256_fmadd_ps(__m256 __a, __m256 __b, __m256 __c) { __m256 __r; __asm__("vmovaps %1, %0\n\tvfmadd213ps %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128 _mm_fmadd_ps(__m128 __a, __m128 __b, __m128 __c) { __m128 __r; __asm__("vmovaps %1, %0\n\tvfmadd213ps %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m256 _mm256_fmsub_ps(__m256 __a, __m256 __b, __m256 __c) { __m256 __r; __asm__("vmovaps %1, %0\n\tvfmsub213ps %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128 _mm_fmsub_ps(__m128 __a, __m128 __b, __m128 __c) { __m128 __r; __asm__("vmovaps %1, %0\n\tvfmsub213ps %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m256 _mm256_fnmadd_ps(__m256 __a, __m256 __b, __m256 __c) { __m256 __r; __asm__("vmovaps %1, %0\n\tvfnmadd213ps %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128 _mm_fnmadd_ps(__m128 __a, __m128 __b, __m128 __c) { __m128 __r; __asm__("vmovaps %1, %0\n\tvfnmadd213ps %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m256 _mm256_fnmsub_ps(__m256 __a, __m256 __b, __m256 __c) { __m256 __r; __asm__("vmovaps %1, %0\n\tvfnmsub213ps %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128 _mm_fnmsub_ps(__m128 __a, __m128 __b, __m128 __c) { __m128 __r; __asm__("vmovaps %1, %0\n\tvfnmsub213ps %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m256 _mm256_fmaddsub_ps(__m256 __a, __m256 __b, __m256 __c) { __m256 __r; __asm__("vmovaps %1, %0\n\tvfmaddsub213ps %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128 _mm_fmaddsub_ps(__m128 __a, __m128 __b, __m128 __c) { __m128 __r; __asm__("vmovaps %1, %0\n\tvfmaddsub213ps %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m256 _mm256_fmsubadd_ps(__m256 __a, __m256 __b, __m256 __c) { __m256 __r; __asm__("vmovaps %1, %0\n\tvfmsubadd213ps %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128 _mm_fmsubadd_ps(__m128 __a, __m128 __b, __m128 __c) { __m128 __r; __asm__("vmovaps %1, %0\n\tvfmsubadd213ps %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128 _mm_fmadd_ss(__m128 __a, __m128 __b, __m128 __c) { __m128 __r; __asm__("vmovaps %1, %0\n\tvfmadd213ss %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128 _mm_fmsub_ss(__m128 __a, __m128 __b, __m128 __c) { __m128 __r; __asm__("vmovaps %1, %0\n\tvfmsub213ss %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128 _mm_fnmadd_ss(__m128 __a, __m128 __b, __m128 __c) { __m128 __r; __asm__("vmovaps %1, %0\n\tvfnmadd213ss %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128 _mm_fnmsub_ss(__m128 __a, __m128 __b, __m128 __c) { __m128 __r; __asm__("vmovaps %1, %0\n\tvfnmsub213ss %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m256d _mm256_add_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vaddpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_sub_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vsubpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_mul_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vmulpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_div_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vdivpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_min_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vminpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_max_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vmaxpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_and_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vandpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_or_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vorpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_xor_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vxorpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_andnot_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vandnpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_unpacklo_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vunpcklpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_unpackhi_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vunpckhpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_hadd_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vhaddpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_hsub_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vhsubpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_addsub_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vaddsubpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_sqrt_pd(__m256d __a) { __m256d __r; __asm__("vsqrtpd %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256d _mm256_permutevar_pd(__m256d __a, __m256d __b) { __m256d __r; __asm__("vpermilpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
#define _mm256_permute_pd(a, imm) __extension__({ __m256d __a = (a); __m256d __r; __asm__("vpermilpd %2, %1, %0" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm256_round_pd(a, imm) __extension__({ __m256d __a = (a); __m256d __r; __asm__("vroundpd %2, %1, %0" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm256_floor_pd(a) _mm256_round_pd(a, _MM_FROUND_FLOOR)
#define _mm256_ceil_pd(a) _mm256_round_pd(a, _MM_FROUND_CEIL)
#define _mm256_shuffle_pd(a, b, imm) __extension__({ __m256d __a = (a), __b = (b), __r; __asm__("vshufpd %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm256_blend_pd(a, b, imm) __extension__({ __m256d __a = (a), __b = (b), __r; __asm__("vblendpd %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm256_cmp_pd(a, b, imm) __extension__({ __m256d __a = (a), __b = (b), __r; __asm__("vcmppd %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm256_permute2f128_pd(a, b, imm) __extension__({ __m256d __a = (a), __b = (b), __r; __asm__("vperm2f128 %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm_cmp_pd(a, b, imm) __extension__({ __m128d __a = (a), __b = (b), __r; __asm__("vcmppd %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm_permute_pd(a, imm) __extension__({ __m128d __a = (a); __m128d __r; __asm__("vpermilpd %2, %1, %0" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
__RI __m128d _mm_permutevar_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("vpermilpd %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_blendv_pd(__m256d __a, __m256d __b, __m256d __m) { __m256d __r; __asm__("vblendvpd %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__m)); return __r; }
__RI int _mm256_testz_pd(__m256d __a, __m256d __b) { int __r; __asm__("vtestpd %2, %1\n\tsete %b0\n\tmovzbl %b0, %k0" : "=r"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI int _mm256_testc_pd(__m256d __a, __m256d __b) { int __r; __asm__("vtestpd %2, %1\n\tsetb %b0\n\tmovzbl %b0, %k0" : "=r"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI int _mm256_testnzc_pd(__m256d __a, __m256d __b) { int __r; __asm__("vtestpd %2, %1\n\tseta %b0\n\tmovzbl %b0, %k0" : "=r"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m256d _mm256_fmadd_pd(__m256d __a, __m256d __b, __m256d __c) { __m256d __r; __asm__("vmovaps %1, %0\n\tvfmadd213pd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128d _mm_fmadd_pd(__m128d __a, __m128d __b, __m128d __c) { __m128d __r; __asm__("vmovaps %1, %0\n\tvfmadd213pd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m256d _mm256_fmsub_pd(__m256d __a, __m256d __b, __m256d __c) { __m256d __r; __asm__("vmovaps %1, %0\n\tvfmsub213pd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128d _mm_fmsub_pd(__m128d __a, __m128d __b, __m128d __c) { __m128d __r; __asm__("vmovaps %1, %0\n\tvfmsub213pd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m256d _mm256_fnmadd_pd(__m256d __a, __m256d __b, __m256d __c) { __m256d __r; __asm__("vmovaps %1, %0\n\tvfnmadd213pd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128d _mm_fnmadd_pd(__m128d __a, __m128d __b, __m128d __c) { __m128d __r; __asm__("vmovaps %1, %0\n\tvfnmadd213pd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m256d _mm256_fnmsub_pd(__m256d __a, __m256d __b, __m256d __c) { __m256d __r; __asm__("vmovaps %1, %0\n\tvfnmsub213pd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128d _mm_fnmsub_pd(__m128d __a, __m128d __b, __m128d __c) { __m128d __r; __asm__("vmovaps %1, %0\n\tvfnmsub213pd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m256d _mm256_fmaddsub_pd(__m256d __a, __m256d __b, __m256d __c) { __m256d __r; __asm__("vmovaps %1, %0\n\tvfmaddsub213pd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128d _mm_fmaddsub_pd(__m128d __a, __m128d __b, __m128d __c) { __m128d __r; __asm__("vmovaps %1, %0\n\tvfmaddsub213pd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m256d _mm256_fmsubadd_pd(__m256d __a, __m256d __b, __m256d __c) { __m256d __r; __asm__("vmovaps %1, %0\n\tvfmsubadd213pd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128d _mm_fmsubadd_pd(__m128d __a, __m128d __b, __m128d __c) { __m128d __r; __asm__("vmovaps %1, %0\n\tvfmsubadd213pd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128d _mm_fmadd_sd(__m128d __a, __m128d __b, __m128d __c) { __m128d __r; __asm__("vmovaps %1, %0\n\tvfmadd213sd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128d _mm_fmsub_sd(__m128d __a, __m128d __b, __m128d __c) { __m128d __r; __asm__("vmovaps %1, %0\n\tvfmsub213sd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128d _mm_fnmadd_sd(__m128d __a, __m128d __b, __m128d __c) { __m128d __r; __asm__("vmovaps %1, %0\n\tvfnmadd213sd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
__RI __m128d _mm_fnmsub_sd(__m128d __a, __m128d __b, __m128d __c) { __m128d __r; __asm__("vmovaps %1, %0\n\tvfnmsub213sd %3, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__c)); return __r; }
#define _mm256_dp_ps(a, b, imm) __extension__({ __m256 __a = (a), __b = (b), __r; __asm__("vdpps %3, %2, %1, %0" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
__RI __m256 _mm256_permutevar8x32_ps(__m256 __a, __m256 __b) { __m256 __r; __asm__("vpermps %1, %2, %0" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
#define _mm256_permute4x64_pd(a, imm) __extension__({ __m256d __a = (a); __m256d __r; __asm__("vpermpd %2, %1, %0" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
__RI __m256 _mm256_rcp_ps(__m256 __a) { __m256 __r; __asm__("vrcpps %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256 _mm256_rsqrt_ps(__m256 __a) { __m256 __r; __asm__("vrsqrtps %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256 _mm256_movehdup_ps(__m256 __a) { __m256 __r; __asm__("vmovshdup %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256 _mm256_moveldup_ps(__m256 __a) { __m256 __r; __asm__("vmovsldup %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256d _mm256_movedup_pd(__m256d __a) { __m256d __r; __asm__("vmovddup %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256 _mm256_cvtepi32_ps(__m256i __a) { __m256 __r; __asm__("vcvtdq2ps %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_cvtps_epi32(__m256 __a) { __m256i __r; __asm__("vcvtps2dq %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256i _mm256_cvttps_epi32(__m256 __a) { __m256i __r; __asm__("vcvttps2dq %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256d _mm256_cvtepi32_pd(__m128i __a) { __m256d __r; __asm__("vcvtdq2pd %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m256d _mm256_cvtps_pd(__m128 __a) { __m256d __r; __asm__("vcvtps2pd %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128 _mm256_cvtpd_ps(__m256d __a) { __m128 __r; __asm__("vcvtpd2ps %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm256_cvtpd_epi32(__m256d __a) { __m128i __r; __asm__("vcvtpd2dq %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm256_cvttpd_epi32(__m256d __a) { __m128i __r; __asm__("vcvttpd2dq %1, %0" : "=x"(__r) : "x"(__a)); return __r; }

/* --- 128 bits (SSE to SSE4.1, in SSE encodings; AVX2/FMA ones above) --- */

__RI __m128i _mm_setzero_si128(void) { __m128i __r; __asm__("{xorps %0, %0|vxorps %0, %0, %0}" : "=x"(__r) : ); return __r; }
__RI __m128 _mm_setzero_ps(void) { __m128 __r; __asm__("{xorps %0, %0|vxorps %0, %0, %0}" : "=x"(__r) : ); return __r; }
__RI __m128d _mm_setzero_pd(void) { __m128d __r; __asm__("{xorps %0, %0|vxorps %0, %0, %0}" : "=x"(__r) : ); return __r; }
__RI __m128i _mm_undefined_si128(void) { return _mm_setzero_si128(); }
__RI __m128i _mm_set1_epi8(char __x) { union __ritteros_v128 __u; for (int __k = 0; __k < 16; __k++) __u.__b[__k] = __x; return __u.__i; }
__RI __m128i _mm_set_epi8(char __e15, char __e14, char __e13, char __e12, char __e11, char __e10, char __e9, char __e8, char __e7, char __e6, char __e5, char __e4, char __e3, char __e2, char __e1, char __e0) { union __ritteros_v128 __u = { .__b = { __e0, __e1, __e2, __e3, __e4, __e5, __e6, __e7, __e8, __e9, __e10, __e11, __e12, __e13, __e14, __e15 } }; return __u.__i; }
__RI __m128i _mm_setr_epi8(char __e0, char __e1, char __e2, char __e3, char __e4, char __e5, char __e6, char __e7, char __e8, char __e9, char __e10, char __e11, char __e12, char __e13, char __e14, char __e15) { union __ritteros_v128 __u = { .__b = { __e0, __e1, __e2, __e3, __e4, __e5, __e6, __e7, __e8, __e9, __e10, __e11, __e12, __e13, __e14, __e15 } }; return __u.__i; }
__RI __m128i _mm_set1_epi16(short __x) { union __ritteros_v128 __u; for (int __k = 0; __k < 8; __k++) __u.__w[__k] = __x; return __u.__i; }
__RI __m128i _mm_set_epi16(short __e7, short __e6, short __e5, short __e4, short __e3, short __e2, short __e1, short __e0) { union __ritteros_v128 __u = { .__w = { __e0, __e1, __e2, __e3, __e4, __e5, __e6, __e7 } }; return __u.__i; }
__RI __m128i _mm_setr_epi16(short __e0, short __e1, short __e2, short __e3, short __e4, short __e5, short __e6, short __e7) { union __ritteros_v128 __u = { .__w = { __e0, __e1, __e2, __e3, __e4, __e5, __e6, __e7 } }; return __u.__i; }
__RI __m128i _mm_set1_epi32(int __x) { __m128i __r; __asm__("{movd %1, %0\n\tpshufd $0, %0, %0|vmovd %1, %0\n\tvpshufd $0, %0, %0}" : "=x"(__r) : "r"(__x)); return __r; }
__RI __m128i _mm_set_epi32(int __e3, int __e2, int __e1, int __e0) { union __ritteros_v128 __u = { .__l = { __e0, __e1, __e2, __e3 } }; return __u.__i; }
__RI __m128i _mm_setr_epi32(int __e0, int __e1, int __e2, int __e3) { union __ritteros_v128 __u = { .__l = { __e0, __e1, __e2, __e3 } }; return __u.__i; }
__RI __m128i _mm_set1_epi64x(long long __x) { union __ritteros_v128 __u; for (int __k = 0; __k < 2; __k++) __u.__q[__k] = __x; return __u.__i; }
__RI __m128i _mm_set_epi64x(long long __e1, long long __e0) { union __ritteros_v128 __u = { .__q = { __e0, __e1 } }; return __u.__i; }
__RI __m128 _mm_set1_ps(float __x) { __m128 __r; __asm__("{movaps %1, %0\n\tshufps $0, %0, %0|vshufps $0, %1, %1, %0}" : "=x"(__r) : "x"(__x)); return __r; }
__RI __m128 _mm_set_ps(float __e3, float __e2, float __e1, float __e0) { union __ritteros_v128 __u = { .__s = { __e0, __e1, __e2, __e3 } }; return __u.__f; }
__RI __m128 _mm_setr_ps(float __e0, float __e1, float __e2, float __e3) { union __ritteros_v128 __u = { .__s = { __e0, __e1, __e2, __e3 } }; return __u.__f; }
__RI __m128d _mm_set1_pd(double __x) { __m128d __r; __asm__("{movaps %1, %0\n\tunpcklpd %0, %0|vmovddup %1, %0}" : "=x"(__r) : "x"(__x)); return __r; }
__RI __m128d _mm_set_pd(double __e1, double __e0) { union __ritteros_v128 __u = { .__e = { __e0, __e1 } }; return __u.__d; }
__RI __m128d _mm_setr_pd(double __e0, double __e1) { union __ritteros_v128 __u = { .__e = { __e0, __e1 } }; return __u.__d; }
__RI __m128 _mm_set_ss(float __x) { union __ritteros_v128 __u = { .__q = { 0 } }; __u.__s[0] = __x; return __u.__f; }
__RI __m128d _mm_set_sd(double __x) { union __ritteros_v128 __u = { .__q = { 0 } }; __u.__e[0] = __x; return __u.__d; }
__RI __m128 _mm_set_ps1(float __x) { return _mm_set1_ps(__x); }
__RI __m128d _mm_set_pd1(double __x) { return _mm_set1_pd(__x); }
__RI __m128i _mm_load_si128(const __m128i *__p) { return *(const __m128i *)__p; }
__RI __m128i _mm_loadu_si128(const __m128i *__p) { return *(const __m128i *)__p; }
__RI void _mm_store_si128(__m128i *__p, __m128i __a) { *(__m128i *)__p = __a; }
__RI void _mm_storeu_si128(__m128i *__p, __m128i __a) { *(__m128i *)__p = __a; }
__RI void _mm_stream_si128(__m128i *__p, __m128i __a) { *(__m128i *)__p = __a; }
__RI __m128 _mm_load_ps(const float *__p) { return *(const __m128 *)__p; }
__RI __m128 _mm_loadu_ps(const float *__p) { return *(const __m128 *)__p; }
__RI void _mm_store_ps(float *__p, __m128 __a) { *(__m128 *)__p = __a; }
__RI void _mm_storeu_ps(float *__p, __m128 __a) { *(__m128 *)__p = __a; }
__RI void _mm_stream_ps(float *__p, __m128 __a) { *(__m128 *)__p = __a; }
__RI __m128d _mm_load_pd(const double *__p) { return *(const __m128d *)__p; }
__RI __m128d _mm_loadu_pd(const double *__p) { return *(const __m128d *)__p; }
__RI void _mm_store_pd(double *__p, __m128d __a) { *(__m128d *)__p = __a; }
__RI void _mm_storeu_pd(double *__p, __m128d __a) { *(__m128d *)__p = __a; }
__RI void _mm_stream_pd(double *__p, __m128d __a) { *(__m128d *)__p = __a; }
__RI __m128i _mm_lddqu_si128(const __m128i *__p) { return *__p; }
__RI __m128i _mm_loadl_epi64(const __m128i *__p) { union __ritteros_v128 __u = { .__q = { 0 } }; __u.__q[0] = *(const long long *)__p; return __u.__i; }
__RI void _mm_storel_epi64(__m128i *__p, __m128i __a) { union __ritteros_v128 __u = { __a }; *(long long *)__p = __u.__q[0]; }
__RI __m128 _mm_load_ss(const float *__p) { return _mm_set_ss(*__p); }
__RI __m128d _mm_load_sd(const double *__p) { return _mm_set_sd(*__p); }
__RI __m128 _mm_load1_ps(const float *__p) { return _mm_set1_ps(*__p); }
__RI __m128d _mm_load1_pd(const double *__p) { return _mm_set1_pd(*__p); }
__RI void _mm_store_ss(float *__p, __m128 __a) { union __ritteros_v128 __u; __u.__f = __a; *__p = __u.__s[0]; }
__RI void _mm_store_sd(double *__p, __m128d __a) { union __ritteros_v128 __u; __u.__d = __a; *__p = __u.__e[0]; }
__RI __m128i _mm_cvtsi32_si128(int __x) { __m128i __r; __asm__("{movd %1, %0|vmovd %1, %0}" : "=x"(__r) : "r"(__x)); return __r; }
__RI __m128i _mm_cvtsi64_si128(long long __x) { __m128i __r; __asm__("{movq %1, %0|vmovq %1, %0}" : "=x"(__r) : "r"(__x)); return __r; }
__RI int _mm_cvtsi128_si32(__m128i __a) { int __r; __asm__("{movd %1, %0|vmovd %1, %0}" : "=r"(__r) : "x"(__a)); return __r; }
__RI long long _mm_cvtsi128_si64(__m128i __a) { long long __r; __asm__("{movq %1, %0|vmovq %1, %0}" : "=r"(__r) : "x"(__a)); return __r; }
__RI float _mm_cvtss_f32(__m128 __a) { float __r; __asm__("{movaps %1, %0|vmovaps %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI double _mm_cvtsd_f64(__m128d __a) { double __r; __asm__("{movaps %1, %0|vmovaps %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128 _mm_castsi128_ps(__m128i __a) { __m128 __r; __asm__("{movaps %1, %0|vmovaps %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128d _mm_castsi128_pd(__m128i __a) { __m128d __r; __asm__("{movaps %1, %0|vmovaps %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_castps_si128(__m128 __a) { __m128i __r; __asm__("{movaps %1, %0|vmovaps %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_castpd_si128(__m128d __a) { __m128i __r; __asm__("{movaps %1, %0|vmovaps %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128d _mm_castps_pd(__m128 __a) { __m128d __r; __asm__("{movaps %1, %0|vmovaps %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128 _mm_castpd_ps(__m128d __a) { __m128 __r; __asm__("{movaps %1, %0|vmovaps %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI int _mm_extract_epi8(__m128i __a, const int __k) { union __ritteros_v128 __u = { __a }; return (unsigned char)__u.__b[__k & 15]; }
__RI int _mm_extract_epi16(__m128i __a, const int __k) { union __ritteros_v128 __u = { __a }; return (unsigned short)__u.__w[__k & 7]; }
__RI int _mm_extract_epi32(__m128i __a, const int __k) { union __ritteros_v128 __u = { __a }; return __u.__l[__k & 3]; }
__RI long long _mm_extract_epi64(__m128i __a, const int __k) { union __ritteros_v128 __u = { __a }; return __u.__q[__k & 1]; }
__RI __m128i _mm_insert_epi8(__m128i __a, int __x, const int __k) { union __ritteros_v128 __u = { __a }; __u.__b[__k & 15] = (signed char)__x; return __u.__i; }
__RI __m128i _mm_insert_epi16(__m128i __a, int __x, const int __k) { union __ritteros_v128 __u = { __a }; __u.__w[__k & 7] = (short)__x; return __u.__i; }
__RI __m128i _mm_insert_epi32(__m128i __a, int __x, const int __k) { union __ritteros_v128 __u = { __a }; __u.__l[__k & 3] = __x; return __u.__i; }
__RI __m128i _mm_insert_epi64(__m128i __a, long long __x, const int __k) { union __ritteros_v128 __u = { __a }; __u.__q[__k & 1] = __x; return __u.__i; }
__RI __m128i _mm_broadcastb_epi8(__m128i __a) { __m128i __r; __asm__("vpbroadcastb %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_broadcastw_epi16(__m128i __a) { __m128i __r; __asm__("vpbroadcastw %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_broadcastd_epi32(__m128i __a) { __m128i __r; __asm__("vpbroadcastd %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_broadcastq_epi64(__m128i __a) { __m128i __r; __asm__("vpbroadcastq %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128 _mm_broadcastss_ps(__m128 __a) { __m128 __r; __asm__("vbroadcastss %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128d _mm_broadcastsd_pd(__m128d __a) { __m128d __r; __asm__("vmovddup %1, %0" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128 _mm_broadcast_ss(const float *__p) { return _mm_set1_ps(*__p); }
__RI void _mm_pause(void) { __asm__ __volatile__("pause"); }
__RI void _mm_mfence(void) { __asm__ __volatile__("mfence" : : : "memory"); }
__RI void _mm_lfence(void) { __asm__ __volatile__("lfence" : : : "memory"); }
__RI void _mm_sfence(void) { __asm__ __volatile__("sfence" : : : "memory"); }
__RI __m128i _mm_add_epi8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpaddb %2, %0|vpaddb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_add_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpaddw %2, %0|vpaddw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_add_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpaddd %2, %0|vpaddd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_add_epi64(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpaddq %2, %0|vpaddq %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_sub_epi8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsubb %2, %0|vpsubb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_sub_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsubw %2, %0|vpsubw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_sub_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsubd %2, %0|vpsubd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_sub_epi64(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsubq %2, %0|vpsubq %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_adds_epi8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpaddsb %2, %0|vpaddsb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_adds_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpaddsw %2, %0|vpaddsw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_adds_epu8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpaddusb %2, %0|vpaddusb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_adds_epu16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpaddusw %2, %0|vpaddusw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_subs_epi8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsubsb %2, %0|vpsubsb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_subs_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsubsw %2, %0|vpsubsw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_subs_epu8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsubusb %2, %0|vpsubusb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_subs_epu16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsubusw %2, %0|vpsubusw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_mullo_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmullw %2, %0|vpmullw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_mullo_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmulld %2, %0|vpmulld %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_mulhi_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmulhw %2, %0|vpmulhw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_mulhi_epu16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmulhuw %2, %0|vpmulhuw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_mul_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmuldq %2, %0|vpmuldq %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_mul_epu32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmuludq %2, %0|vpmuludq %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_madd_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmaddwd %2, %0|vpmaddwd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_maddubs_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmaddubsw %2, %0|vpmaddubsw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_sad_epu8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsadbw %2, %0|vpsadbw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_avg_epu8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpavgb %2, %0|vpavgb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_avg_epu16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpavgw %2, %0|vpavgw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_mulhrs_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmulhrsw %2, %0|vpmulhrsw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_min_epi8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpminsb %2, %0|vpminsb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_min_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpminsw %2, %0|vpminsw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_min_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpminsd %2, %0|vpminsd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_min_epu8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpminub %2, %0|vpminub %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_min_epu16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpminuw %2, %0|vpminuw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_min_epu32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpminud %2, %0|vpminud %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_max_epi8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmaxsb %2, %0|vpmaxsb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_max_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmaxsw %2, %0|vpmaxsw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_max_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmaxsd %2, %0|vpmaxsd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_max_epu8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmaxub %2, %0|vpmaxub %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_max_epu16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmaxuw %2, %0|vpmaxuw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_max_epu32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpmaxud %2, %0|vpmaxud %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_sign_epi8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsignb %2, %0|vpsignb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_sign_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsignw %2, %0|vpsignw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_sign_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsignd %2, %0|vpsignd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_hadd_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tphaddw %2, %0|vphaddw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_hadd_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tphaddd %2, %0|vphaddd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_hadds_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tphaddsw %2, %0|vphaddsw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_hsub_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tphsubw %2, %0|vphsubw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_hsub_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tphsubd %2, %0|vphsubd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_hsubs_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tphsubsw %2, %0|vphsubsw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_and_si128(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpand %2, %0|vpand %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_or_si128(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpor %2, %0|vpor %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_xor_si128(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpxor %2, %0|vpxor %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_andnot_si128(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpandn %2, %0|vpandn %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_cmpeq_epi8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpcmpeqb %2, %0|vpcmpeqb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_cmpeq_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpcmpeqw %2, %0|vpcmpeqw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_cmpeq_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpcmpeqd %2, %0|vpcmpeqd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_cmpeq_epi64(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpcmpeqq %2, %0|vpcmpeqq %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_cmpgt_epi8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpcmpgtb %2, %0|vpcmpgtb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_cmpgt_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpcmpgtw %2, %0|vpcmpgtw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_cmpgt_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpcmpgtd %2, %0|vpcmpgtd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_cmpgt_epi64(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpcmpgtq %2, %0|vpcmpgtq %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_shuffle_epi8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpshufb %2, %0|vpshufb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_unpacklo_epi8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpunpcklbw %2, %0|vpunpcklbw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_unpacklo_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpunpcklwd %2, %0|vpunpcklwd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_unpacklo_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpunpckldq %2, %0|vpunpckldq %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_unpacklo_epi64(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpunpcklqdq %2, %0|vpunpcklqdq %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_unpackhi_epi8(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpunpckhbw %2, %0|vpunpckhbw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_unpackhi_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpunpckhwd %2, %0|vpunpckhwd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_unpackhi_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpunpckhdq %2, %0|vpunpckhdq %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_unpackhi_epi64(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpunpckhqdq %2, %0|vpunpckhqdq %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_packs_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpacksswb %2, %0|vpacksswb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_packs_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpackssdw %2, %0|vpackssdw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_packus_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpackuswb %2, %0|vpackuswb %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_packus_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpackusdw %2, %0|vpackusdw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_cmplt_epi8(__m128i __a, __m128i __b) { return _mm_cmpgt_epi8(__b, __a); }
__RI __m128i _mm_cmplt_epi16(__m128i __a, __m128i __b) { return _mm_cmpgt_epi16(__b, __a); }
__RI __m128i _mm_cmplt_epi32(__m128i __a, __m128i __b) { return _mm_cmpgt_epi32(__b, __a); }
__RI __m128i _mm_slli_epi16(__m128i __a, int __n) { __m128i __r; int __c = __n & 255; __asm__("{movd %k2, %%xmm7\n\tmovdqa %1, %0\n\tpsllw %%xmm7, %0|vmovd %k2, %x0\n\tvpsllw %x0, %1, %0}" : "=x"(__r) : "x"(__a), "r"(__c) : "xmm7"); return __r; }
__RI __m128i _mm_slli_epi32(__m128i __a, int __n) { __m128i __r; int __c = __n & 255; __asm__("{movd %k2, %%xmm7\n\tmovdqa %1, %0\n\tpslld %%xmm7, %0|vmovd %k2, %x0\n\tvpslld %x0, %1, %0}" : "=x"(__r) : "x"(__a), "r"(__c) : "xmm7"); return __r; }
__RI __m128i _mm_slli_epi64(__m128i __a, int __n) { __m128i __r; int __c = __n & 255; __asm__("{movd %k2, %%xmm7\n\tmovdqa %1, %0\n\tpsllq %%xmm7, %0|vmovd %k2, %x0\n\tvpsllq %x0, %1, %0}" : "=x"(__r) : "x"(__a), "r"(__c) : "xmm7"); return __r; }
__RI __m128i _mm_srli_epi16(__m128i __a, int __n) { __m128i __r; int __c = __n & 255; __asm__("{movd %k2, %%xmm7\n\tmovdqa %1, %0\n\tpsrlw %%xmm7, %0|vmovd %k2, %x0\n\tvpsrlw %x0, %1, %0}" : "=x"(__r) : "x"(__a), "r"(__c) : "xmm7"); return __r; }
__RI __m128i _mm_srli_epi32(__m128i __a, int __n) { __m128i __r; int __c = __n & 255; __asm__("{movd %k2, %%xmm7\n\tmovdqa %1, %0\n\tpsrld %%xmm7, %0|vmovd %k2, %x0\n\tvpsrld %x0, %1, %0}" : "=x"(__r) : "x"(__a), "r"(__c) : "xmm7"); return __r; }
__RI __m128i _mm_srli_epi64(__m128i __a, int __n) { __m128i __r; int __c = __n & 255; __asm__("{movd %k2, %%xmm7\n\tmovdqa %1, %0\n\tpsrlq %%xmm7, %0|vmovd %k2, %x0\n\tvpsrlq %x0, %1, %0}" : "=x"(__r) : "x"(__a), "r"(__c) : "xmm7"); return __r; }
__RI __m128i _mm_srai_epi16(__m128i __a, int __n) { __m128i __r; int __c = __n & 255; __asm__("{movd %k2, %%xmm7\n\tmovdqa %1, %0\n\tpsraw %%xmm7, %0|vmovd %k2, %x0\n\tvpsraw %x0, %1, %0}" : "=x"(__r) : "x"(__a), "r"(__c) : "xmm7"); return __r; }
__RI __m128i _mm_srai_epi32(__m128i __a, int __n) { __m128i __r; int __c = __n & 255; __asm__("{movd %k2, %%xmm7\n\tmovdqa %1, %0\n\tpsrad %%xmm7, %0|vmovd %k2, %x0\n\tvpsrad %x0, %1, %0}" : "=x"(__r) : "x"(__a), "r"(__c) : "xmm7"); return __r; }
__RI __m128i _mm_sll_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsllw %2, %0|vpsllw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_sll_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpslld %2, %0|vpslld %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_sll_epi64(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsllq %2, %0|vpsllq %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_srl_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsrlw %2, %0|vpsrlw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_srl_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsrld %2, %0|vpsrld %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_srl_epi64(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsrlq %2, %0|vpsrlq %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_sra_epi16(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsraw %2, %0|vpsraw %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_sra_epi32(__m128i __a, __m128i __b) { __m128i __r; __asm__("{movdqa %1, %0\n\tpsrad %2, %0|vpsrad %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128i _mm_abs_epi8(__m128i __a) { __m128i __r; __asm__("{pabsb %1, %0|vpabsb %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_abs_epi16(__m128i __a) { __m128i __r; __asm__("{pabsw %1, %0|vpabsw %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_abs_epi32(__m128i __a) { __m128i __r; __asm__("{pabsd %1, %0|vpabsd %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_cvtepi8_epi16(__m128i __a) { __m128i __r; __asm__("{pmovsxbw %1, %0|vpmovsxbw %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_cvtepi8_epi32(__m128i __a) { __m128i __r; __asm__("{pmovsxbd %1, %0|vpmovsxbd %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_cvtepi8_epi64(__m128i __a) { __m128i __r; __asm__("{pmovsxbq %1, %0|vpmovsxbq %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_cvtepi16_epi32(__m128i __a) { __m128i __r; __asm__("{pmovsxwd %1, %0|vpmovsxwd %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_cvtepi16_epi64(__m128i __a) { __m128i __r; __asm__("{pmovsxwq %1, %0|vpmovsxwq %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_cvtepi32_epi64(__m128i __a) { __m128i __r; __asm__("{pmovsxdq %1, %0|vpmovsxdq %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_cvtepu8_epi16(__m128i __a) { __m128i __r; __asm__("{pmovzxbw %1, %0|vpmovzxbw %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_cvtepu8_epi32(__m128i __a) { __m128i __r; __asm__("{pmovzxbd %1, %0|vpmovzxbd %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_cvtepu8_epi64(__m128i __a) { __m128i __r; __asm__("{pmovzxbq %1, %0|vpmovzxbq %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_cvtepu16_epi32(__m128i __a) { __m128i __r; __asm__("{pmovzxwd %1, %0|vpmovzxwd %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_cvtepu16_epi64(__m128i __a) { __m128i __r; __asm__("{pmovzxwq %1, %0|vpmovzxwq %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_cvtepu32_epi64(__m128i __a) { __m128i __r; __asm__("{pmovzxdq %1, %0|vpmovzxdq %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_minpos_epu16(__m128i __a) { __m128i __r; __asm__("{phminposuw %1, %0|vphminposuw %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI int _mm_movemask_epi8(__m128i __a) { int __r; __asm__("{pmovmskb %1, %0|vpmovmskb %1, %0}" : "=r"(__r) : "x"(__a)); return __r; }
__RI int _mm_movemask_ps(__m128 __a) { int __r; __asm__("{movmskps %1, %0|vmovmskps %1, %0}" : "=r"(__r) : "x"(__a)); return __r; }
__RI int _mm_movemask_pd(__m128d __a) { int __r; __asm__("{movmskpd %1, %0|vmovmskpd %1, %0}" : "=r"(__r) : "x"(__a)); return __r; }
__RI int _mm_testz_si128(__m128i __a, __m128i __b) { int __r; __asm__("{ptest %2, %1\n\tsete %b0\n\tmovzbl %b0, %k0|vptest %2, %1\n\tsete %b0\n\tmovzbl %b0, %k0}" : "=r"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI int _mm_testc_si128(__m128i __a, __m128i __b) { int __r; __asm__("{ptest %2, %1\n\tsetb %b0\n\tmovzbl %b0, %k0|vptest %2, %1\n\tsetb %b0\n\tmovzbl %b0, %k0}" : "=r"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI int _mm_testnzc_si128(__m128i __a, __m128i __b) { int __r; __asm__("{ptest %2, %1\n\tseta %b0\n\tmovzbl %b0, %k0|vptest %2, %1\n\tseta %b0\n\tmovzbl %b0, %k0}" : "=r"(__r) : "x"(__a), "x"(__b)); return __r; }
#define _mm_shuffle_epi32(a, imm) __extension__({ __m128i __a = (a); __m128i __r; __asm__("{pshufd %2, %1, %0|vpshufd %2, %1, %0}" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm_shufflehi_epi16(a, imm) __extension__({ __m128i __a = (a); __m128i __r; __asm__("{pshufhw %2, %1, %0|vpshufhw %2, %1, %0}" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm_shufflelo_epi16(a, imm) __extension__({ __m128i __a = (a); __m128i __r; __asm__("{pshuflw %2, %1, %0|vpshuflw %2, %1, %0}" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm_slli_si128(a, imm) __extension__({ __m128i __a = (a), __r; __asm__("{movdqa %1, %0\n\tpslldq %2, %0|vpslldq %2, %1, %0}" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm_srli_si128(a, imm) __extension__({ __m128i __a = (a), __r; __asm__("{movdqa %1, %0\n\tpsrldq %2, %0|vpsrldq %2, %1, %0}" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm_bslli_si128(a, imm) _mm_slli_si128(a, imm)
#define _mm_bsrli_si128(a, imm) _mm_srli_si128(a, imm)
#define _mm_alignr_epi8(a, b, imm) __extension__({ __m128i __a = (a), __b = (b), __r; __asm__("{movdqa %1, %0\n\tpalignr %3, %2, %0|vpalignr %3, %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm_blend_epi16(a, b, imm) __extension__({ __m128i __a = (a), __b = (b), __r; __asm__("{movdqa %1, %0\n\tpblendw %3, %2, %0|vpblendw %3, %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
__RI __m128i _mm_blendv_epi8(__m128i __a, __m128i __b, __m128i __m) { __m128i __r; __asm__("{movdqa %3, %%xmm0\n\tmovdqa %1, %0\n\tpblendvb %%xmm0, %2, %0|vpblendvb %3, %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__m) : "xmm0"); return __r; }
__RI __m128 _mm_add_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\taddps %2, %0|vaddps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_sub_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tsubps %2, %0|vsubps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_mul_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tmulps %2, %0|vmulps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_div_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tdivps %2, %0|vdivps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_min_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tminps %2, %0|vminps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_max_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tmaxps %2, %0|vmaxps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_and_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tandps %2, %0|vandps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_or_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\torps %2, %0|vorps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_xor_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\txorps %2, %0|vxorps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_andnot_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tandnps %2, %0|vandnps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_unpacklo_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tunpcklps %2, %0|vunpcklps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_unpackhi_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tunpckhps %2, %0|vunpckhps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_hadd_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\thaddps %2, %0|vhaddps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_hsub_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\thsubps %2, %0|vhsubps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_addsub_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\taddsubps %2, %0|vaddsubps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_sqrt_ps(__m128 __a) { __m128 __r; __asm__("{sqrtps %1, %0|vsqrtps %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
#define _mm_shuffle_ps(a, b, imm) __extension__({ __m128 __a = (a), __b = (b), __r; __asm__("{movaps %1, %0\n\tshufps %3, %2, %0|vshufps %3, %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm_blend_ps(a, b, imm) __extension__({ __m128 __a = (a), __b = (b), __r; __asm__("{movaps %1, %0\n\tblendps %3, %2, %0|vblendps %3, %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm_round_ps(a, imm) __extension__({ __m128 __a = (a); __m128 __r; __asm__("{roundps %2, %1, %0|vroundps %2, %1, %0}" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm_floor_ps(a) _mm_round_ps(a, _MM_FROUND_FLOOR)
#define _mm_ceil_ps(a) _mm_round_ps(a, _MM_FROUND_CEIL)
__RI __m128 _mm_blendv_ps(__m128 __a, __m128 __b, __m128 __m) { __m128 __r; __asm__("{movaps %3, %%xmm0\n\tmovaps %1, %0\n\tblendvps %%xmm0, %2, %0|vblendvps %3, %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__m) : "xmm0"); return __r; }
__RI __m128 _mm_cmpeq_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tcmpeqps %2, %0|vcmpeqps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_cmplt_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tcmpltps %2, %0|vcmpltps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_cmple_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tcmpleps %2, %0|vcmpleps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_cmpunord_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tcmpunordps %2, %0|vcmpunordps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_cmpneq_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tcmpneqps %2, %0|vcmpneqps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_cmpnlt_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tcmpnltps %2, %0|vcmpnltps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_cmpnle_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tcmpnleps %2, %0|vcmpnleps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_cmpord_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tcmpordps %2, %0|vcmpordps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_cmpgt_ps(__m128 __a, __m128 __b) { return _mm_cmplt_ps(__b, __a); }
__RI __m128 _mm_cmpge_ps(__m128 __a, __m128 __b) { return _mm_cmple_ps(__b, __a); }
__RI __m128 _mm_add_ss(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\taddss %2, %0|vaddss %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_sub_ss(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tsubss %2, %0|vsubss %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_mul_ss(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tmulss %2, %0|vmulss %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_div_ss(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tdivss %2, %0|vdivss %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_min_ss(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tminss %2, %0|vminss %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_max_ss(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tmaxss %2, %0|vmaxss %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_add_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\taddpd %2, %0|vaddpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_sub_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tsubpd %2, %0|vsubpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_mul_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tmulpd %2, %0|vmulpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_div_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tdivpd %2, %0|vdivpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_min_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tminpd %2, %0|vminpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_max_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tmaxpd %2, %0|vmaxpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_and_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tandpd %2, %0|vandpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_or_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\torpd %2, %0|vorpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_xor_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\txorpd %2, %0|vxorpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_andnot_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tandnpd %2, %0|vandnpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_unpacklo_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tunpcklpd %2, %0|vunpcklpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_unpackhi_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tunpckhpd %2, %0|vunpckhpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_hadd_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\thaddpd %2, %0|vhaddpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_hsub_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\thsubpd %2, %0|vhsubpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_addsub_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\taddsubpd %2, %0|vaddsubpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_sqrt_pd(__m128d __a) { __m128d __r; __asm__("{sqrtpd %1, %0|vsqrtpd %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
#define _mm_shuffle_pd(a, b, imm) __extension__({ __m128d __a = (a), __b = (b), __r; __asm__("{movaps %1, %0\n\tshufpd %3, %2, %0|vshufpd %3, %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm_blend_pd(a, b, imm) __extension__({ __m128d __a = (a), __b = (b), __r; __asm__("{movaps %1, %0\n\tblendpd %3, %2, %0|vblendpd %3, %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm_round_pd(a, imm) __extension__({ __m128d __a = (a); __m128d __r; __asm__("{roundpd %2, %1, %0|vroundpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "i"(imm)); __r; })
#define _mm_floor_pd(a) _mm_round_pd(a, _MM_FROUND_FLOOR)
#define _mm_ceil_pd(a) _mm_round_pd(a, _MM_FROUND_CEIL)
__RI __m128d _mm_blendv_pd(__m128d __a, __m128d __b, __m128d __m) { __m128d __r; __asm__("{movaps %3, %%xmm0\n\tmovaps %1, %0\n\tblendvpd %%xmm0, %2, %0|vblendvpd %3, %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b), "x"(__m) : "xmm0"); return __r; }
__RI __m128d _mm_cmpeq_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tcmpeqpd %2, %0|vcmpeqpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_cmplt_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tcmpltpd %2, %0|vcmpltpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_cmple_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tcmplepd %2, %0|vcmplepd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_cmpunord_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tcmpunordpd %2, %0|vcmpunordpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_cmpneq_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tcmpneqpd %2, %0|vcmpneqpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_cmpnlt_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tcmpnltpd %2, %0|vcmpnltpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_cmpnle_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tcmpnlepd %2, %0|vcmpnlepd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_cmpord_pd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tcmpordpd %2, %0|vcmpordpd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_cmpgt_pd(__m128d __a, __m128d __b) { return _mm_cmplt_pd(__b, __a); }
__RI __m128d _mm_cmpge_pd(__m128d __a, __m128d __b) { return _mm_cmple_pd(__b, __a); }
__RI __m128d _mm_add_sd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\taddsd %2, %0|vaddsd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_sub_sd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tsubsd %2, %0|vsubsd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_mul_sd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tmulsd %2, %0|vmulsd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_div_sd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tdivsd %2, %0|vdivsd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_min_sd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tminsd %2, %0|vminsd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128d _mm_max_sd(__m128d __a, __m128d __b) { __m128d __r; __asm__("{movaps %1, %0\n\tmaxsd %2, %0|vmaxsd %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
#define _mm_dp_ps(a, b, imm) __extension__({ __m128 __a = (a), __b = (b), __r; __asm__("{movaps %1, %0\n\tdpps %3, %2, %0|vdpps %3, %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
#define _mm_dp_pd(a, b, imm) __extension__({ __m128d __a = (a), __b = (b), __r; __asm__("{movaps %1, %0\n\tdppd %3, %2, %0|vdppd %3, %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b), "i"(imm)); __r; })
__RI __m128 _mm_movehl_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tmovhlps %2, %0|vmovhlps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_movelh_ps(__m128 __a, __m128 __b) { __m128 __r; __asm__("{movaps %1, %0\n\tmovlhps %2, %0|vmovlhps %2, %1, %0}" : "=x"(__r) : "x"(__a), "x"(__b)); return __r; }
__RI __m128 _mm_rcp_ps(__m128 __a) { __m128 __r; __asm__("{rcpps %1, %0|vrcpps %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128 _mm_rsqrt_ps(__m128 __a) { __m128 __r; __asm__("{rsqrtps %1, %0|vrsqrtps %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128 _mm_movehdup_ps(__m128 __a) { __m128 __r; __asm__("{movshdup %1, %0|vmovshdup %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128 _mm_moveldup_ps(__m128 __a) { __m128 __r; __asm__("{movsldup %1, %0|vmovsldup %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128d _mm_movedup_pd(__m128d __a) { __m128d __r; __asm__("{movddup %1, %0|vmovddup %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128 _mm_cvtepi32_ps(__m128i __a) { __m128 __r; __asm__("{cvtdq2ps %1, %0|vcvtdq2ps %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_cvtps_epi32(__m128 __a) { __m128i __r; __asm__("{cvtps2dq %1, %0|vcvtps2dq %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128i _mm_cvttps_epi32(__m128 __a) { __m128i __r; __asm__("{cvttps2dq %1, %0|vcvttps2dq %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128d _mm_cvtepi32_pd(__m128i __a) { __m128d __r; __asm__("{cvtdq2pd %1, %0|vcvtdq2pd %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }
__RI __m128d _mm_cvtps_pd(__m128 __a) { __m128d __r; __asm__("{cvtps2pd %1, %0|vcvtps2pd %1, %0}" : "=x"(__r) : "x"(__a)); return __r; }

#undef __RI

#endif
