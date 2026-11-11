/*
 * (32h) cc_shell_test compiles this with RitterOS's cc and reads the
 * assembly: fir's vectors stay in ymm registers, its scalar float code is
 * VEX (no SSE encoding next to dirty ymm registers), it clears the upper
 * halves before returning, and plain (no target attribute) neither gets
 * fir inlined nor any VEX instruction.
 */
#include <immintrin.h>

float h[32], x[1024 + 32], y[1024];

__attribute__((target("avx2,fma"))) static void fir(void)
{
    for (int i = 0; i < 1024; i += 8) {
        __m256 s = _mm256_setzero_ps();
        for (int k = 0; k < 32; k++)
            s = _mm256_fmadd_ps(_mm256_loadu_ps(&x[i + k]), _mm256_set1_ps(h[k] * 0.5f), s);
        _mm256_storeu_ps(&y[i], s);
    }
}

float plain(int avx)
{
    float t = 0;
    if (avx)
        fir();
    for (int i = 0; i < 1024; i++)
        t += y[i] * 2.0f;
    return t;
}
