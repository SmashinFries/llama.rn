#define _CRT_SECURE_NO_DEPRECATE // Disables ridiculous "unsafe" warnigns on Windows

#include "ggml.h"

#ifdef LM_GGML_USE_K_QUANTS
#include "k_quants.h"
#endif

#if defined(_MSC_VER) || defined(__MINGW32__)
#include <malloc.h> // using malloc.h with MSC/MINGW
#elif !defined(__FreeBSD__) && !defined(__NetBSD__) && !defined(__OpenBSD__)
#include <alloca.h>
#endif

#include <assert.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <float.h>
#include <limits.h>
#include <stdarg.h>
#include <signal.h>

#ifdef LM_GGML_USE_METAL
#include <unistd.h>
#endif

// static_assert should be a #define, but if it's not,
// fall back to the _Static_assert C11 keyword.
// if C99 - static_assert is noop
// ref: https://stackoverflow.com/a/53923785/4039976
#ifndef static_assert
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201100L)
#define static_assert(cond, msg) _Static_assert(cond, msg)
#else
#define static_assert(cond, msg) struct global_scope_noop_trick
#endif
#endif

#if defined(_MSC_VER)
// disable "possible loss of data" to avoid hundreds of casts
// we should just be careful :)
#pragma warning(disable: 4244 4267)

// disable POSIX deprecation warnigns
// these functions are never going away, anyway
#pragma warning(disable: 4996)
#endif

#if defined(_WIN32)

#include <windows.h>

typedef volatile LONG atomic_int;
typedef atomic_int atomic_bool;

static void atomic_store(atomic_int * ptr, LONG val) {
    InterlockedExchange(ptr, val);
}
static LONG atomic_load(atomic_int * ptr) {
    return InterlockedCompareExchange(ptr, 0, 0);
}
static LONG atomic_fetch_add(atomic_int * ptr, LONG inc) {
    return InterlockedExchangeAdd(ptr, inc);
}
static LONG atomic_fetch_sub(atomic_int * ptr, LONG dec) {
    return atomic_fetch_add(ptr, -(dec));
}

typedef HANDLE pthread_t;

typedef DWORD thread_ret_t;
static int pthread_create(pthread_t * out, void * unused, thread_ret_t(*func)(void *), void * arg) {
    (void) unused;
    HANDLE handle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) func, arg, 0, NULL);
    if (handle == NULL)
    {
        return EAGAIN;
    }

    *out = handle;
    return 0;
}

static int pthread_join(pthread_t thread, void * unused) {
    (void) unused;
    int ret = (int) WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return ret;
}

static int sched_yield (void) {
    Sleep (0);
    return 0;
}
#else
#include <pthread.h>
#include <stdatomic.h>

typedef void * thread_ret_t;

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#endif
#ifdef LM_GGML_USE_CPU_HBM
#include <hbwmalloc.h>
#endif

// __FMA__ and __F16C__ are not defined in MSVC, however they are implied with AVX2/AVX512
#if defined(_MSC_VER) && (defined(__AVX2__) || defined(__AVX512F__))
#ifndef __FMA__
#define __FMA__
#endif
#ifndef __F16C__
#define __F16C__
#endif
#ifndef __SSE3__
#define __SSE3__
#endif
#endif

/*#define LM_GGML_PERF*/
#define LM_GGML_DEBUG 0
#define LM_GGML_GELU_FP16
#define LM_GGML_GELU_QUICK_FP16
#define LM_GGML_SILU_FP16
// #define LM_GGML_CROSS_ENTROPY_EXP_FP16
// #define LM_GGML_FLASH_ATTN_EXP_FP16

#define LM_GGML_SOFT_MAX_UNROLL 4
#define LM_GGML_VEC_DOT_UNROLL  2
#define LM_GGML_VEC_MAD_UNROLL  32

//
// logging
//

#if (LM_GGML_DEBUG >= 1)
#define LM_GGML_PRINT_DEBUG(...) printf(__VA_ARGS__)
#else
#define LM_GGML_PRINT_DEBUG(...)
#endif

#if (LM_GGML_DEBUG >= 5)
#define LM_GGML_PRINT_DEBUG_5(...) printf(__VA_ARGS__)
#else
#define LM_GGML_PRINT_DEBUG_5(...)
#endif

#if (LM_GGML_DEBUG >= 10)
#define LM_GGML_PRINT_DEBUG_10(...) printf(__VA_ARGS__)
#else
#define LM_GGML_PRINT_DEBUG_10(...)
#endif

#define LM_GGML_PRINT(...) printf(__VA_ARGS__)

//
// end of logging block
//

#ifdef LM_GGML_USE_ACCELERATE
// uncomment to use vDSP for soft max computation
// note: not sure if it is actually faster
//#define LM_GGML_SOFT_MAX_ACCELERATE
#endif

#if defined(_MSC_VER) || defined(__MINGW32__)
#define LM_GGML_ALIGNED_MALLOC(size) _aligned_malloc(size, LM_GGML_MEM_ALIGN)
#define LM_GGML_ALIGNED_FREE(ptr)    _aligned_free(ptr)
#else
inline static void * lm_ggml_aligned_malloc(size_t size) {
    if (size == 0) {
        LM_GGML_PRINT("WARNING: Behavior may be unexpected when allocating 0 bytes for lm_ggml_aligned_malloc!\n");
        return NULL;
    }
    void * aligned_memory = NULL;
#ifdef LM_GGML_USE_CPU_HBM
    int result = hbw_posix_memalign(&aligned_memory, 16, size);
#elif LM_GGML_USE_METAL
    int result = posix_memalign(&aligned_memory, sysconf(_SC_PAGESIZE), size);
#else
    int result = posix_memalign(&aligned_memory, LM_GGML_MEM_ALIGN, size);
#endif
    if (result != 0) {
        // Handle allocation failure
        const char *error_desc = "unknown allocation error";
        switch (result) {
            case EINVAL:
                error_desc = "invalid alignment value";
                break;
            case ENOMEM:
                error_desc = "insufficient memory";
                break;
        }
        LM_GGML_PRINT("%s: %s (attempted to allocate %6.2f MB)\n", __func__, error_desc, size/(1024.0*1024.0));
        return NULL;
    }
    return aligned_memory;
}
#define LM_GGML_ALIGNED_MALLOC(size) lm_ggml_aligned_malloc(size)
#ifdef LM_GGML_USE_CPU_HBM
#define LM_GGML_ALIGNED_FREE(ptr)    if(NULL != ptr) hbw_free(ptr)
#else
#define LM_GGML_ALIGNED_FREE(ptr)    free(ptr)
#endif
#endif

#define UNUSED LM_GGML_UNUSED
#define SWAP(x, y, T) do { T SWAP = x; x = y; y = SWAP; } while (0)

//
// tensor access macros
//

#define LM_GGML_TENSOR_UNARY_OP_LOCALS \
    LM_GGML_TENSOR_LOCALS(int64_t, ne0, src0, ne) \
    LM_GGML_TENSOR_LOCALS(size_t,  nb0, src0, nb) \
    LM_GGML_TENSOR_LOCALS(int64_t, ne,  dst,  ne) \
    LM_GGML_TENSOR_LOCALS(size_t,  nb,  dst,  nb)

#define LM_GGML_TENSOR_BINARY_OP_LOCALS \
    LM_GGML_TENSOR_LOCALS(int64_t, ne0, src0, ne) \
    LM_GGML_TENSOR_LOCALS(size_t,  nb0, src0, nb) \
    LM_GGML_TENSOR_LOCALS(int64_t, ne1, src1, ne) \
    LM_GGML_TENSOR_LOCALS(size_t,  nb1, src1, nb) \
    LM_GGML_TENSOR_LOCALS(int64_t, ne,  dst,  ne) \
    LM_GGML_TENSOR_LOCALS(size_t,  nb,  dst,  nb)

#if defined(LM_GGML_USE_ACCELERATE)
#include <Accelerate/Accelerate.h>
#if defined(LM_GGML_USE_CLBLAST) // allow usage of CLBlast alongside Accelerate functions
#include "ggml-opencl.h"
#endif
#elif defined(LM_GGML_USE_OPENBLAS)
#if defined(LM_GGML_BLAS_USE_MKL)
#include <mkl.h>
#else
#include <cblas.h>
#endif
#elif defined(LM_GGML_USE_CUBLAS)
#include "ggml-cuda.h"
#elif defined(LM_GGML_USE_CLBLAST)
#include "ggml-opencl.h"
#endif

#undef MIN
#undef MAX
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// floating point type used to accumulate sums
typedef double lm_ggml_float;

// 16-bit float
// on Arm, we use __fp16
// on x86, we use uint16_t
#if defined(__ARM_NEON) && !defined(_MSC_VER)

// if YCM cannot find <arm_neon.h>, make a symbolic link to it, for example:
//
//   $ ln -sfn /Library/Developer/CommandLineTools/usr/lib/clang/13.1.6/include/arm_neon.h ./src/
//
#include <arm_neon.h>

#define LM_GGML_COMPUTE_FP16_TO_FP32(x) ((float) (x))
#define LM_GGML_COMPUTE_FP32_TO_FP16(x) (x)

#define LM_GGML_FP16_TO_FP32(x) ((float) (x))
#define LM_GGML_FP32_TO_FP16(x) (x)

#else

#ifdef __wasm_simd128__
#include <wasm_simd128.h>
#else
#ifdef __POWER9_VECTOR__
#include <altivec.h>
#undef bool
#define bool _Bool
#else
#if defined(_MSC_VER) || defined(__MINGW32__)
#include <intrin.h>
#else
#if defined(__AVX__) || defined(__AVX2__) || defined(__AVX512F__) || defined(__SSSE3__) || defined(__SSE3__)
#if !defined(__riscv)
#include <immintrin.h>
#endif
#endif
#endif
#endif
#endif

#ifdef __riscv_v_intrinsic
#include <riscv_vector.h>
#endif

#ifdef __F16C__

#ifdef _MSC_VER
#define LM_GGML_COMPUTE_FP16_TO_FP32(x) _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(x)))
#define LM_GGML_COMPUTE_FP32_TO_FP16(x) _mm_extract_epi16(_mm_cvtps_ph(_mm_set_ss(x), 0), 0)
#else
#define LM_GGML_COMPUTE_FP16_TO_FP32(x) _cvtsh_ss(x)
#define LM_GGML_COMPUTE_FP32_TO_FP16(x) _cvtss_sh(x, 0)
#endif

#elif defined(__POWER9_VECTOR__)

#define LM_GGML_COMPUTE_FP16_TO_FP32(x) lm_ggml_compute_fp16_to_fp32(x)
#define LM_GGML_COMPUTE_FP32_TO_FP16(x) lm_ggml_compute_fp32_to_fp16(x)
/* the inline asm below is about 12% faster than the lookup method */
#define LM_GGML_FP16_TO_FP32(x) LM_GGML_COMPUTE_FP16_TO_FP32(x)
#define LM_GGML_FP32_TO_FP16(x) LM_GGML_COMPUTE_FP32_TO_FP16(x)

static inline float lm_ggml_compute_fp16_to_fp32(lm_ggml_fp16_t h) {
    register float f;
    register double d;
    __asm__(
        "mtfprd %0,%2\n"
        "xscvhpdp %0,%0\n"
        "frsp %1,%0\n" :
        /* temp */ "=d"(d),
        /* out */  "=f"(f):
        /* in */   "r"(h));
    return f;
}

static inline lm_ggml_fp16_t lm_ggml_compute_fp32_to_fp16(float f) {
    register double d;
    register lm_ggml_fp16_t r;
    __asm__( /* xscvdphp can work on double or single precision */
        "xscvdphp %0,%2\n"
        "mffprd %1,%0\n" :
        /* temp */ "=d"(d),
        /* out */  "=r"(r):
        /* in */   "f"(f));
    return r;
}

#else

// FP16 <-> FP32
// ref: https://github.com/Maratyszcza/FP16

static inline float fp32_from_bits(uint32_t w) {
    union {
        uint32_t as_bits;
        float as_value;
    } fp32;
    fp32.as_bits = w;
    return fp32.as_value;
}

static inline uint32_t fp32_to_bits(float f) {
    union {
        float as_value;
        uint32_t as_bits;
    } fp32;
    fp32.as_value = f;
    return fp32.as_bits;
}

static inline float lm_ggml_compute_fp16_to_fp32(lm_ggml_fp16_t h) {
    const uint32_t w = (uint32_t) h << 16;
    const uint32_t sign = w & UINT32_C(0x80000000);
    const uint32_t two_w = w + w;

    const uint32_t exp_offset = UINT32_C(0xE0) << 23;
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) || defined(__GNUC__) && !defined(__STRICT_ANSI__)
    const float exp_scale = 0x1.0p-112f;
#else
    const float exp_scale = fp32_from_bits(UINT32_C(0x7800000));
#endif
    const float normalized_value = fp32_from_bits((two_w >> 4) + exp_offset) * exp_scale;

    const uint32_t magic_mask = UINT32_C(126) << 23;
    const float magic_bias = 0.5f;
    const float denormalized_value = fp32_from_bits((two_w >> 17) | magic_mask) - magic_bias;

    const uint32_t denormalized_cutoff = UINT32_C(1) << 27;
    const uint32_t result = sign |
        (two_w < denormalized_cutoff ? fp32_to_bits(denormalized_value) : fp32_to_bits(normalized_value));
    return fp32_from_bits(result);
}

static inline lm_ggml_fp16_t lm_ggml_compute_fp32_to_fp16(float f) {
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) || defined(__GNUC__) && !defined(__STRICT_ANSI__)
    const float scale_to_inf = 0x1.0p+112f;
    const float scale_to_zero = 0x1.0p-110f;
#else
    const float scale_to_inf = fp32_from_bits(UINT32_C(0x77800000));
    const float scale_to_zero = fp32_from_bits(UINT32_C(0x08800000));
#endif
    float base = (fabsf(f) * scale_to_inf) * scale_to_zero;

    const uint32_t w = fp32_to_bits(f);
    const uint32_t shl1_w = w + w;
    const uint32_t sign = w & UINT32_C(0x80000000);
    uint32_t bias = shl1_w & UINT32_C(0xFF000000);
    if (bias < UINT32_C(0x71000000)) {
        bias = UINT32_C(0x71000000);
    }

    base = fp32_from_bits((bias >> 1) + UINT32_C(0x07800000)) + base;
    const uint32_t bits = fp32_to_bits(base);
    const uint32_t exp_bits = (bits >> 13) & UINT32_C(0x00007C00);
    const uint32_t mantissa_bits = bits & UINT32_C(0x00000FFF);
    const uint32_t nonsign = exp_bits + mantissa_bits;
    return (sign >> 16) | (shl1_w > UINT32_C(0xFF000000) ? UINT16_C(0x7E00) : nonsign);
}

#define LM_GGML_COMPUTE_FP16_TO_FP32(x) lm_ggml_compute_fp16_to_fp32(x)
#define LM_GGML_COMPUTE_FP32_TO_FP16(x) lm_ggml_compute_fp32_to_fp16(x)

#endif // __F16C__

#endif // __ARM_NEON

//
// global data
//

// precomputed gelu table for f16 (128 KB)
static lm_ggml_fp16_t table_gelu_f16[1 << 16];

// precomputed quick gelu table for f16 (128 KB)
static lm_ggml_fp16_t table_gelu_quick_f16[1 << 16];

// precomputed silu table for f16 (128 KB)
static lm_ggml_fp16_t table_silu_f16[1 << 16];

// precomputed exp table for f16 (128 KB)
static lm_ggml_fp16_t table_exp_f16[1 << 16];

// precomputed f32 table for f16 (256 KB)
static float table_f32_f16[1 << 16];

#if defined(__ARM_NEON) || defined(__wasm_simd128__)
#define B1(c,s,n)  0x ## n ## c ,  0x ## n ## s
#define B2(c,s,n) B1(c,s,n ## c), B1(c,s,n ## s)
#define B3(c,s,n) B2(c,s,n ## c), B2(c,s,n ## s)
#define B4(c,s,n) B3(c,s,n ## c), B3(c,s,n ## s)
#define B5(c,s,n) B4(c,s,n ## c), B4(c,s,n ## s)
#define B6(c,s,n) B5(c,s,n ## c), B5(c,s,n ## s)
#define B7(c,s,n) B6(c,s,n ## c), B6(c,s,n ## s)
#define B8(c,s  ) B7(c,s,     c), B7(c,s,     s)

// precomputed tables for expanding 8bits to 8 bytes:
static const uint64_t table_b2b_0[1 << 8] = { B8(00, 10) }; // ( b) << 4
static const uint64_t table_b2b_1[1 << 8] = { B8(10, 00) }; // (!b) << 4
#endif

// On ARM NEON, it's quicker to directly convert x -> x instead of calling into lm_ggml_lookup_fp16_to_fp32,
// so we define LM_GGML_FP16_TO_FP32 and LM_GGML_FP32_TO_FP16 elsewhere for NEON.
// This is also true for POWER9.
#if !defined(LM_GGML_FP16_TO_FP32) || !defined(LM_GGML_FP32_TO_FP16)

inline static float lm_ggml_lookup_fp16_to_fp32(lm_ggml_fp16_t f) {
    uint16_t s;
    memcpy(&s, &f, sizeof(uint16_t));
    return table_f32_f16[s];
}

#define LM_GGML_FP16_TO_FP32(x) lm_ggml_lookup_fp16_to_fp32(x)
#define LM_GGML_FP32_TO_FP16(x) LM_GGML_COMPUTE_FP32_TO_FP16(x)

#endif

// note: do not use these inside ggml.c
// these are meant to be used via the ggml.h API
float lm_ggml_fp16_to_fp32(lm_ggml_fp16_t x) {
    return (float) LM_GGML_FP16_TO_FP32(x);
}

lm_ggml_fp16_t lm_ggml_fp32_to_fp16(float x) {
    return LM_GGML_FP32_TO_FP16(x);
}

void lm_ggml_fp16_to_fp32_row(const lm_ggml_fp16_t * x, float * y, int n) {
    for (int i = 0; i < n; i++) {
        y[i] = LM_GGML_FP16_TO_FP32(x[i]);
    }
}

void lm_ggml_fp32_to_fp16_row(const float * x, lm_ggml_fp16_t * y, int n) {
    int i = 0;
#if defined(__F16C__)
    for (; i + 7 < n; i += 8) {
        __m256 x_vec = _mm256_loadu_ps(x + i);
        __m128i y_vec = _mm256_cvtps_ph(x_vec, _MM_FROUND_TO_NEAREST_INT);
        _mm_storeu_si128((__m128i *)(y + i), y_vec);
    }
    for(; i + 3 < n; i += 4) {
        __m128 x_vec = _mm_loadu_ps(x + i);
        __m128i y_vec = _mm_cvtps_ph(x_vec, _MM_FROUND_TO_NEAREST_INT);
        _mm_storel_epi64((__m128i *)(y + i), y_vec);
    }
#endif
    for (; i < n; i++) {
        y[i] = LM_GGML_FP32_TO_FP16(x[i]);
    }
}

//
// timing
//

#if defined(_MSC_VER) || defined(__MINGW32__)
static int64_t timer_freq, timer_start;
void lm_ggml_time_init(void) {
    LARGE_INTEGER t;
    QueryPerformanceFrequency(&t);
    timer_freq = t.QuadPart;

    // The multiplication by 1000 or 1000000 below can cause an overflow if timer_freq
    // and the uptime is high enough.
    // We subtract the program start time to reduce the likelihood of that happening.
    QueryPerformanceCounter(&t);
    timer_start = t.QuadPart;
}
int64_t lm_ggml_time_ms(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return ((t.QuadPart-timer_start) * 1000) / timer_freq;
}
int64_t lm_ggml_time_us(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return ((t.QuadPart-timer_start) * 1000000) / timer_freq;
}
#else
void lm_ggml_time_init(void) {}
int64_t lm_ggml_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec*1000 + (int64_t)ts.tv_nsec/1000000;
}

int64_t lm_ggml_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec*1000000 + (int64_t)ts.tv_nsec/1000;
}
#endif

int64_t lm_ggml_cycles(void) {
    return clock();
}

int64_t lm_ggml_cycles_per_ms(void) {
    return CLOCKS_PER_SEC/1000;
}

#ifdef LM_GGML_PERF
#define lm_ggml_perf_time_ms()       lm_ggml_time_ms()
#define lm_ggml_perf_time_us()       lm_ggml_time_us()
#define lm_ggml_perf_cycles()        lm_ggml_cycles()
#define lm_ggml_perf_cycles_per_ms() lm_ggml_cycles_per_ms()
#else
#define lm_ggml_perf_time_ms()       0
#define lm_ggml_perf_time_us()       0
#define lm_ggml_perf_cycles()        0
#define lm_ggml_perf_cycles_per_ms() 0
#endif

//
// cache line
//

#if defined(__cpp_lib_hardware_interference_size)
#define CACHE_LINE_SIZE hardware_destructive_interference_size
#else
#if defined(__POWER9_VECTOR__)
#define CACHE_LINE_SIZE 128
#else
#define CACHE_LINE_SIZE 64
#endif
#endif

static const size_t CACHE_LINE_SIZE_F32 = CACHE_LINE_SIZE/sizeof(float);

//
// quantization
//

#define MM256_SET_M128I(a, b) _mm256_insertf128_si256(_mm256_castsi128_si256(b), (a), 1)

#if defined(__AVX__) || defined(__AVX2__) || defined(__AVX512F__) || defined(__SSSE3__)
// multiply int8_t, add results pairwise twice
static inline __m128i mul_sum_i8_pairs(const __m128i x, const __m128i y) {
    // Get absolute values of x vectors
    const __m128i ax = _mm_sign_epi8(x, x);
    // Sign the values of the y vectors
    const __m128i sy = _mm_sign_epi8(y, x);
    // Perform multiplication and create 16-bit values
    const __m128i dot = _mm_maddubs_epi16(ax, sy);
    const __m128i ones = _mm_set1_epi16(1);
    return _mm_madd_epi16(ones, dot);
}

#if __AVX__ || __AVX2__ || __AVX512F__
// horizontally add 8 floats
static inline float hsum_float_8(const __m256 x) {
    __m128 res = _mm256_extractf128_ps(x, 1);
    res = _mm_add_ps(res, _mm256_castps256_ps128(x));
    res = _mm_add_ps(res, _mm_movehl_ps(res, res));
    res = _mm_add_ss(res, _mm_movehdup_ps(res));
    return _mm_cvtss_f32(res);
}

// horizontally add 8 int32_t
static inline int hsum_i32_8(const __m256i a) {
    const __m128i sum128 = _mm_add_epi32(_mm256_castsi256_si128(a), _mm256_extractf128_si256(a, 1));
    const __m128i hi64 = _mm_unpackhi_epi64(sum128, sum128);
    const __m128i sum64 = _mm_add_epi32(hi64, sum128);
    const __m128i hi32  = _mm_shuffle_epi32(sum64, _MM_SHUFFLE(2, 3, 0, 1));
    return _mm_cvtsi128_si32(_mm_add_epi32(sum64, hi32));
}

// horizontally add 4 int32_t
static inline int hsum_i32_4(const __m128i a) {
    const __m128i hi64 = _mm_unpackhi_epi64(a, a);
    const __m128i sum64 = _mm_add_epi32(hi64, a);
    const __m128i hi32  = _mm_shuffle_epi32(sum64, _MM_SHUFFLE(2, 3, 0, 1));
    return _mm_cvtsi128_si32(_mm_add_epi32(sum64, hi32));
}

#if defined(__AVX2__) || defined(__AVX512F__)
// spread 32 bits to 32 bytes { 0x00, 0xFF }
static inline __m256i bytes_from_bits_32(const uint8_t * x) {
    uint32_t x32;
    memcpy(&x32, x, sizeof(uint32_t));
    const __m256i shuf_mask = _mm256_set_epi64x(
            0x0303030303030303, 0x0202020202020202,
            0x0101010101010101, 0x0000000000000000);
    __m256i bytes = _mm256_shuffle_epi8(_mm256_set1_epi32(x32), shuf_mask);
    const __m256i bit_mask = _mm256_set1_epi64x(0x7fbfdfeff7fbfdfe);
    bytes = _mm256_or_si256(bytes, bit_mask);
    return _mm256_cmpeq_epi8(bytes, _mm256_set1_epi64x(-1));
}

// Unpack 32 4-bit fields into 32 bytes
// The output vector contains 32 bytes, each one in [ 0 .. 15 ] interval
static inline __m256i bytes_from_nibbles_32(const uint8_t * rsi)
{
    const __m128i tmp = _mm_loadu_si128((const __m128i *)rsi);
    const __m256i bytes = MM256_SET_M128I(_mm_srli_epi16(tmp, 4), tmp);
    const __m256i lowMask = _mm256_set1_epi8( 0xF );
    return _mm256_and_si256(lowMask, bytes);
}

// add int16_t pairwise and return as float vector
static inline __m256 sum_i16_pairs_float(const __m256i x) {
    const __m256i ones = _mm256_set1_epi16(1);
    const __m256i summed_pairs = _mm256_madd_epi16(ones, x);
    return _mm256_cvtepi32_ps(summed_pairs);
}

static inline __m256 mul_sum_us8_pairs_float(const __m256i ax, const __m256i sy) {
#if __AVXVNNI__
    const __m256i zero = _mm256_setzero_si256();
    const __m256i summed_pairs = _mm256_dpbusd_epi32(zero, ax, sy);
    return _mm256_cvtepi32_ps(summed_pairs);
#else
    // Perform multiplication and create 16-bit values
    const __m256i dot = _mm256_maddubs_epi16(ax, sy);
    return sum_i16_pairs_float(dot);
#endif
}

// multiply int8_t, add results pairwise twice and return as float vector
static inline __m256 mul_sum_i8_pairs_float(const __m256i x, const __m256i y) {
#if __AVXVNNIINT8__
    const __m256i zero = _mm256_setzero_si256();
    const __m256i summed_pairs = _mm256_dpbssd_epi32(zero, x, y);
    return _mm256_cvtepi32_ps(summed_pairs);
#else
    // Get absolute values of x vectors
    const __m256i ax = _mm256_sign_epi8(x, x);
    // Sign the values of the y vectors
    const __m256i sy = _mm256_sign_epi8(y, x);
    return mul_sum_us8_pairs_float(ax, sy);
#endif
}

static inline __m128i packNibbles( __m256i bytes )
{
    // Move bits within 16-bit lanes from 0000_abcd_0000_efgh into 0000_0000_abcd_efgh
#if __AVX512F__
    const __m256i bytes_srli_4 = _mm256_srli_epi16(bytes, 4);   // 0000_0000_abcd_0000
    bytes = _mm256_or_si256(bytes, bytes_srli_4);               // 0000_abcd_abcd_efgh
    return _mm256_cvtepi16_epi8(bytes);                         // abcd_efgh
#else
    const __m256i lowByte = _mm256_set1_epi16( 0xFF );
    __m256i high = _mm256_andnot_si256( lowByte, bytes );
    __m256i low = _mm256_and_si256( lowByte, bytes );
    high = _mm256_srli_epi16( high, 4 );
    bytes = _mm256_or_si256( low, high );

    // Compress uint16_t lanes into bytes
    __m128i r0 = _mm256_castsi256_si128( bytes );
    __m128i r1 = _mm256_extracti128_si256( bytes, 1 );
    return _mm_packus_epi16( r0, r1 );
#endif
}
#elif defined(__AVX__)
// spread 32 bits to 32 bytes { 0x00, 0xFF }
static inline __m256i bytes_from_bits_32(const uint8_t * x) {
    uint32_t x32;
    memcpy(&x32, x, sizeof(uint32_t));
    const __m128i shuf_maskl = _mm_set_epi64x(0x0101010101010101, 0x0000000000000000);
    const __m128i shuf_maskh = _mm_set_epi64x(0x0303030303030303, 0x0202020202020202);
    __m128i bytesl = _mm_shuffle_epi8(_mm_set1_epi32(x32), shuf_maskl);
    __m128i bytesh = _mm_shuffle_epi8(_mm_set1_epi32(x32), shuf_maskh);
    const __m128i bit_mask = _mm_set1_epi64x(0x7fbfdfeff7fbfdfe);
    bytesl = _mm_or_si128(bytesl, bit_mask);
    bytesh = _mm_or_si128(bytesh, bit_mask);
    bytesl = _mm_cmpeq_epi8(bytesl, _mm_set1_epi64x(-1));
    bytesh = _mm_cmpeq_epi8(bytesh, _mm_set1_epi64x(-1));
    return MM256_SET_M128I(bytesh, bytesl);
}

// Unpack 32 4-bit fields into 32 bytes
// The output vector contains 32 bytes, each one in [ 0 .. 15 ] interval
static inline __m256i bytes_from_nibbles_32(const uint8_t * rsi)
{
    // Load 16 bytes from memory
    __m128i tmpl = _mm_loadu_si128((const __m128i *)rsi);
    __m128i tmph = _mm_srli_epi16(tmpl, 4);
    const __m128i lowMask = _mm_set1_epi8(0xF);
    tmpl = _mm_and_si128(lowMask, tmpl);
    tmph = _mm_and_si128(lowMask, tmph);
    return MM256_SET_M128I(tmph, tmpl);
}

// add int16_t pairwise and return as float vector
static inline __m256 sum_i16_pairs_float(const __m128i xh, const __m128i xl) {
    const __m128i ones = _mm_set1_epi16(1);
    const __m128i summed_pairsl = _mm_madd_epi16(ones, xl);
    const __m128i summed_pairsh = _mm_madd_epi16(ones, xh);
    const __m256i summed_pairs = MM256_SET_M128I(summed_pairsh, summed_pairsl);
    return _mm256_cvtepi32_ps(summed_pairs);
}

static inline __m256 mul_sum_us8_pairs_float(const __m256i ax, const __m256i sy) {
    const __m128i axl = _mm256_castsi256_si128(ax);
    const __m128i axh = _mm256_extractf128_si256(ax, 1);
    const __m128i syl = _mm256_castsi256_si128(sy);
    const __m128i syh = _mm256_extractf128_si256(sy, 1);
    // Perform multiplication and create 16-bit values
    const __m128i dotl = _mm_maddubs_epi16(axl, syl);
    const __m128i doth = _mm_maddubs_epi16(axh, syh);
    return sum_i16_pairs_float(doth, dotl);
}

// multiply int8_t, add results pairwise twice and return as float vector
static inline __m256 mul_sum_i8_pairs_float(const __m256i x, const __m256i y) {
    const __m128i xl = _mm256_castsi256_si128(x);
    const __m128i xh = _mm256_extractf128_si256(x, 1);
    const __m128i yl = _mm256_castsi256_si128(y);
    const __m128i yh = _mm256_extractf128_si256(y, 1);
    // Get absolute values of x vectors
    const __m128i axl = _mm_sign_epi8(xl, xl);
    const __m128i axh = _mm_sign_epi8(xh, xh);
    // Sign the values of the y vectors
    const __m128i syl = _mm_sign_epi8(yl, xl);
    const __m128i syh = _mm_sign_epi8(yh, xh);
    // Perform multiplication and create 16-bit values
    const __m128i dotl = _mm_maddubs_epi16(axl, syl);
    const __m128i doth = _mm_maddubs_epi16(axh, syh);
    return sum_i16_pairs_float(doth, dotl);
}

static inline __m128i packNibbles( __m128i bytes1, __m128i bytes2 )
{
    // Move bits within 16-bit lanes from 0000_abcd_0000_efgh into 0000_0000_abcd_efgh
    const __m128i lowByte = _mm_set1_epi16( 0xFF );
    __m128i high = _mm_andnot_si128( lowByte, bytes1 );
    __m128i low = _mm_and_si128( lowByte, bytes1 );
    high = _mm_srli_epi16( high, 4 );
    bytes1 = _mm_or_si128( low, high );
    high = _mm_andnot_si128( lowByte, bytes2 );
    low = _mm_and_si128( lowByte, bytes2 );
    high = _mm_srli_epi16( high, 4 );
    bytes2 = _mm_or_si128( low, high );

    return _mm_packus_epi16( bytes1, bytes2);
}
#endif
#elif defined(__SSSE3__)
// horizontally add 4x4 floats
static inline float hsum_float_4x4(const __m128 a, const __m128 b, const __m128 c, const __m128 d) {
    __m128 res_0 =_mm_hadd_ps(a, b);
    __m128 res_1 =_mm_hadd_ps(c, d);
    __m128 res =_mm_hadd_ps(res_0, res_1);
    res =_mm_hadd_ps(res, res);
    res =_mm_hadd_ps(res, res);

    return _mm_cvtss_f32(res);
}
#endif // __AVX__ || __AVX2__ || __AVX512F__
#endif // defined(__AVX__) || defined(__AVX2__) || defined(__AVX512F__) || defined(__SSSE3__)

#if defined(__ARM_NEON)

#if !defined(__aarch64__)

inline static int32_t vaddvq_s32(int32x4_t v) {
    return vgetq_lane_s32(v, 0) + vgetq_lane_s32(v, 1) + vgetq_lane_s32(v, 2) + vgetq_lane_s32(v, 3);
}

inline static float vaddvq_f32(float32x4_t v) {
    return vgetq_lane_f32(v, 0) + vgetq_lane_f32(v, 1) + vgetq_lane_f32(v, 2) + vgetq_lane_f32(v, 3);
}

inline static float vmaxvq_f32(float32x4_t v) {
    return
        MAX(MAX(vgetq_lane_f32(v, 0), vgetq_lane_f32(v, 1)),
            MAX(vgetq_lane_f32(v, 2), vgetq_lane_f32(v, 3)));
}

inline static int32x4_t vcvtnq_s32_f32(float32x4_t v) {
    int32x4_t res;

    res[0] = roundf(vgetq_lane_f32(v, 0));
    res[1] = roundf(vgetq_lane_f32(v, 1));
    res[2] = roundf(vgetq_lane_f32(v, 2));
    res[3] = roundf(vgetq_lane_f32(v, 3));

    return res;
}

#endif
#endif

#define QK4_0 32
typedef struct {
    lm_ggml_fp16_t d;          // delta
    uint8_t qs[QK4_0 / 2];  // nibbles / quants
} block_q4_0;
static_assert(sizeof(block_q4_0) == sizeof(lm_ggml_fp16_t) + QK4_0 / 2, "wrong q4_0 block size/padding");

#define QK4_1 32
typedef struct {
    lm_ggml_fp16_t d;          // delta
    lm_ggml_fp16_t m;          // min
    uint8_t qs[QK4_1 / 2];  // nibbles / quants
} block_q4_1;
static_assert(sizeof(block_q4_1) == 2 * sizeof(lm_ggml_fp16_t) + QK4_1 / 2, "wrong q4_1 block size/padding");

#define QK5_0 32
typedef struct {
    lm_ggml_fp16_t d;         // delta
    uint8_t qh[4];         // 5-th bit of quants
    uint8_t qs[QK5_0 / 2]; // nibbles / quants
} block_q5_0;
static_assert(sizeof(block_q5_0) == sizeof(lm_ggml_fp16_t) + sizeof(uint32_t) + QK5_0 / 2, "wrong q5_0 block size/padding");

#define QK5_1 32
typedef struct {
    lm_ggml_fp16_t d;         // delta
    lm_ggml_fp16_t m;         // min
    uint8_t qh[4];         // 5-th bit of quants
    uint8_t qs[QK5_1 / 2]; // nibbles / quants
} block_q5_1;
static_assert(sizeof(block_q5_1) == 2 * sizeof(lm_ggml_fp16_t) + sizeof(uint32_t) + QK5_1 / 2, "wrong q5_1 block size/padding");

#define QK8_0 32
typedef struct {
    lm_ggml_fp16_t d;         // delta
    int8_t  qs[QK8_0];     // quants
} block_q8_0;
static_assert(sizeof(block_q8_0) == sizeof(lm_ggml_fp16_t) + QK8_0, "wrong q8_0 block size/padding");

#define QK8_1 32
typedef struct {
    float d;               // delta
    float s;               // d * sum(qs[i])
    int8_t  qs[QK8_1];     // quants
} block_q8_1;
static_assert(sizeof(block_q8_1) == 2*sizeof(float) + QK8_1, "wrong q8_1 block size/padding");

// reference implementation for deterministic creation of model files
static void quantize_row_q4_0_reference(const float * restrict x, block_q4_0 * restrict y, int k) {
    static const int qk = QK4_0;

    assert(k % qk == 0);

    const int nb = k / qk;

    for (int i = 0; i < nb; i++) {
        float amax = 0.0f; // absolute max
        float max  = 0.0f;

        for (int j = 0; j < qk; j++) {
            const float v = x[i*qk + j];
            if (amax < fabsf(v)) {
                amax = fabsf(v);
                max  = v;
            }
        }

        const float d  = max / -8;
        const float id = d ? 1.0f/d : 0.0f;

        y[i].d = LM_GGML_FP32_TO_FP16(d);

        for (int j = 0; j < qk/2; ++j) {
            const float x0 = x[i*qk + 0    + j]*id;
            const float x1 = x[i*qk + qk/2 + j]*id;

            const uint8_t xi0 = MIN(15, (int8_t)(x0 + 8.5f));
            const uint8_t xi1 = MIN(15, (int8_t)(x1 + 8.5f));

            y[i].qs[j]  = xi0;
            y[i].qs[j] |= xi1 << 4;
        }
    }
}

static void quantize_row_q4_0(const float * restrict x, void * restrict y, int k) {
    quantize_row_q4_0_reference(x, y, k);
}

static void quantize_row_q4_1_reference(const float * restrict x, block_q4_1 * restrict y, int k) {
    const int qk = QK4_1;

    assert(k % qk == 0);

    const int nb = k / qk;

    for (int i = 0; i < nb; i++) {
        float min = FLT_MAX;
        float max = -FLT_MAX;

        for (int j = 0; j < qk; j++) {
            const float v = x[i*qk + j];

            if (v < min) min = v;
            if (v > max) max = v;
        }

        const float d  = (max - min) / ((1 << 4) - 1);
        const float id = d ? 1.0f/d : 0.0f;

        y[i].d = LM_GGML_FP32_TO_FP16(d);
        y[i].m = LM_GGML_FP32_TO_FP16(min);

        for (int j = 0; j < qk/2; ++j) {
            const float x0 = (x[i*qk + 0    + j] - min)*id;
            const float x1 = (x[i*qk + qk/2 + j] - min)*id;

            const uint8_t xi0 = MIN(15, (int8_t)(x0 + 0.5f));
            const uint8_t xi1 = MIN(15, (int8_t)(x1 + 0.5f));

            y[i].qs[j]  = xi0;
            y[i].qs[j] |= xi1 << 4;
        }
    }
}

static void quantize_row_q4_1(const float * restrict x, void * restrict y, int k) {
    quantize_row_q4_1_reference(x, y, k);
}

static void quantize_row_q5_0_reference(const float * restrict x, block_q5_0 * restrict y, int k) {
    static const int qk = QK5_0;

    assert(k % qk == 0);

    const int nb = k / qk;

    for (int i = 0; i < nb; i++) {
        float amax = 0.0f; // absolute max
        float max  = 0.0f;

        for (int j = 0; j < qk; j++) {
            const float v = x[i*qk + j];
            if (amax < fabsf(v)) {
                amax = fabsf(v);
                max  = v;
            }
        }

        const float d  = max / -16;
        const float id = d ? 1.0f/d : 0.0f;

        y[i].d = LM_GGML_FP32_TO_FP16(d);

        uint32_t qh = 0;

        for (int j = 0; j < qk/2; ++j) {
            const float x0 = x[i*qk + 0    + j]*id;
            const float x1 = x[i*qk + qk/2 + j]*id;

            const uint8_t xi0 = MIN(31, (int8_t)(x0 + 16.5f));
            const uint8_t xi1 = MIN(31, (int8_t)(x1 + 16.5f));

            y[i].qs[j] = (xi0 & 0x0F) | ((xi1 & 0x0F) << 4);

            // get the 5-th bit and store it in qh at the right position
            qh |= ((xi0 & 0x10u) >> 4) << (j + 0);
            qh |= ((xi1 & 0x10u) >> 4) << (j + qk/2);
        }

        memcpy(&y[i].qh, &qh, sizeof(qh));
    }
}

static void quantize_row_q5_0(const float * restrict x, void * restrict y, int k) {
    quantize_row_q5_0_reference(x, y, k);
}

static void quantize_row_q5_1_reference(const float * restrict x, block_q5_1 * restrict y, int k) {
    const int qk = QK5_1;

    assert(k % qk == 0);

    const int nb = k / qk;

    for (int i = 0; i < nb; i++) {
        float min = FLT_MAX;
        float max = -FLT_MAX;

        for (int j = 0; j < qk; j++) {
            const float v = x[i*qk + j];

            if (v < min) min = v;
            if (v > max) max = v;
        }

        const float d  = (max - min) / ((1 << 5) - 1);
        const float id = d ? 1.0f/d : 0.0f;

        y[i].d = LM_GGML_FP32_TO_FP16(d);
        y[i].m = LM_GGML_FP32_TO_FP16(min);

        uint32_t qh = 0;

        for (int j = 0; j < qk/2; ++j) {
            const float x0 = (x[i*qk + 0    + j] - min)*id;
            const float x1 = (x[i*qk + qk/2 + j] - min)*id;

            const uint8_t xi0 = (uint8_t)(x0 + 0.5f);
            const uint8_t xi1 = (uint8_t)(x1 + 0.5f);

            y[i].qs[j] = (xi0 & 0x0F) | ((xi1 & 0x0F) << 4);

            // get the 5-th bit and store it in qh at the right position
            qh |= ((xi0 & 0x10u) >> 4) << (j + 0);
            qh |= ((xi1 & 0x10u) >> 4) << (j + qk/2);
        }

        memcpy(&y[i].qh, &qh, sizeof(y[i].qh));
    }
}

static void quantize_row_q5_1(const float * restrict x, void * restrict y, int k) {
    quantize_row_q5_1_reference(x, y, k);
}

// reference implementation for deterministic creation of model files
static void quantize_row_q8_0_reference(const float * restrict x, block_q8_0 * restrict y, int k) {
    assert(k % QK8_0 == 0);
    const int nb = k / QK8_0;

    for (int i = 0; i < nb; i++) {
        float amax = 0.0f; // absolute max

        for (int j = 0; j < QK8_0; j++) {
            const float v = x[i*QK8_0 + j];
            amax = MAX(amax, fabsf(v));
        }

        const float d = amax / ((1 << 7) - 1);
        const float id = d ? 1.0f/d : 0.0f;

        y[i].d = LM_GGML_FP32_TO_FP16(d);

        for (int j = 0; j < QK8_0; ++j) {
            const float x0 = x[i*QK8_0 + j]*id;

            y[i].qs[j] = roundf(x0);
        }
    }
}

static void quantize_row_q8_0(const float * restrict x, void * restrict vy, int k) {
    assert(QK8_0 == 32);
    assert(k % QK8_0 == 0);
    const int nb = k / QK8_0;

    block_q8_0 * restrict y = vy;

#if defined(__ARM_NEON)
    for (int i = 0; i < nb; i++) {
        float32x4_t srcv [8];
        float32x4_t asrcv[8];
        float32x4_t amaxv[8];

        for (int j = 0; j < 8; j++) srcv[j]  = vld1q_f32(x + i*32 + 4*j);
        for (int j = 0; j < 8; j++) asrcv[j] = vabsq_f32(srcv[j]);

        for (int j = 0; j < 4; j++) amaxv[2*j] = vmaxq_f32(asrcv[2*j], asrcv[2*j+1]);
        for (int j = 0; j < 2; j++) amaxv[4*j] = vmaxq_f32(amaxv[4*j], amaxv[4*j+2]);
        for (int j = 0; j < 1; j++) amaxv[8*j] = vmaxq_f32(amaxv[8*j], amaxv[8*j+4]);

        const float amax = vmaxvq_f32(amaxv[0]);

        const float d = amax / ((1 << 7) - 1);
        const float id = d ? 1.0f/d : 0.0f;

        y[i].d = LM_GGML_FP32_TO_FP16(d);

        for (int j = 0; j < 8; j++) {
            const float32x4_t v  = vmulq_n_f32(srcv[j], id);
            const int32x4_t   vi = vcvtnq_s32_f32(v);

            y[i].qs[4*j + 0] = vgetq_lane_s32(vi, 0);
            y[i].qs[4*j + 1] = vgetq_lane_s32(vi, 1);
            y[i].qs[4*j + 2] = vgetq_lane_s32(vi, 2);
            y[i].qs[4*j + 3] = vgetq_lane_s32(vi, 3);
        }
    }
#elif defined(__wasm_simd128__)
    for (int i = 0; i < nb; i++) {
        v128_t srcv [8];
        v128_t asrcv[8];
        v128_t amaxv[8];

        for (int j = 0; j < 8; j++) srcv[j]  = wasm_v128_load(x + i*32 + 4*j);
        for (int j = 0; j < 8; j++) asrcv[j] = wasm_f32x4_abs(srcv[j]);

        for (int j = 0; j < 4; j++) amaxv[2*j] = wasm_f32x4_max(asrcv[2*j], asrcv[2*j+1]);
        for (int j = 0; j < 2; j++) amaxv[4*j] = wasm_f32x4_max(amaxv[4*j], amaxv[4*j+2]);
        for (int j = 0; j < 1; j++) amaxv[8*j] = wasm_f32x4_max(amaxv[8*j], amaxv[8*j+4]);

        const float amax = MAX(MAX(wasm_f32x4_extract_lane(amaxv[0], 0),
                                   wasm_f32x4_extract_lane(amaxv[0], 1)),
                               MAX(wasm_f32x4_extract_lane(amaxv[0], 2),
                                   wasm_f32x4_extract_lane(amaxv[0], 3)));

        const float d = amax / ((1 << 7) - 1);
        const float id = d ? 1.0f/d : 0.0f;

        y[i].d = LM_GGML_FP32_TO_FP16(d);

        for (int j = 0; j < 8; j++) {
            const v128_t v  = wasm_f32x4_mul(srcv[j], wasm_f32x4_splat(id));
            const v128_t vi = wasm_i32x4_trunc_sat_f32x4(v);

            y[i].qs[4*j + 0] = wasm_i32x4_extract_lane(vi, 0);
            y[i].qs[4*j + 1] = wasm_i32x4_extract_lane(vi, 1);
            y[i].qs[4*j + 2] = wasm_i32x4_extract_lane(vi, 2);
            y[i].qs[4*j + 3] = wasm_i32x4_extract_lane(vi, 3);
        }
    }
#elif defined(__AVX2__) || defined(__AVX__)
    for (int i = 0; i < nb; i++) {
        // Load elements into 4 AVX vectors
        __m256 v0 = _mm256_loadu_ps( x );
        __m256 v1 = _mm256_loadu_ps( x + 8 );
        __m256 v2 = _mm256_loadu_ps( x + 16 );
        __m256 v3 = _mm256_loadu_ps( x + 24 );
        x += 32;

        // Compute max(abs(e)) for the block
        const __m256 signBit = _mm256_set1_ps( -0.0f );
        __m256 maxAbs = _mm256_andnot_ps( signBit, v0 );
        maxAbs = _mm256_max_ps( maxAbs, _mm256_andnot_ps( signBit, v1 ) );
        maxAbs = _mm256_max_ps( maxAbs, _mm256_andnot_ps( signBit, v2 ) );
        maxAbs = _mm256_max_ps( maxAbs, _mm256_andnot_ps( signBit, v3 ) );

        __m128 max4 = _mm_max_ps( _mm256_extractf128_ps( maxAbs, 1 ), _mm256_castps256_ps128( maxAbs ) );
        max4 = _mm_max_ps( max4, _mm_movehl_ps( max4, max4 ) );
        max4 = _mm_max_ss( max4, _mm_movehdup_ps( max4 ) );
        const float maxScalar = _mm_cvtss_f32( max4 );

        // Quantize these floats
        const float d = maxScalar / 127.f;
        y[i].d = LM_GGML_FP32_TO_FP16(d);
        const float id = ( maxScalar != 0.0f ) ? 127.f / maxScalar : 0.0f;
        const __m256 mul = _mm256_set1_ps( id );

        // Apply the multiplier
        v0 = _mm256_mul_ps( v0, mul );
        v1 = _mm256_mul_ps( v1, mul );
        v2 = _mm256_mul_ps( v2, mul );
        v3 = _mm256_mul_ps( v3, mul );

        // Round to nearest integer
        v0 = _mm256_round_ps( v0, _MM_ROUND_NEAREST );
        v1 = _mm256_round_ps( v1, _MM_ROUND_NEAREST );
        v2 = _mm256_round_ps( v2, _MM_ROUND_NEAREST );
        v3 = _mm256_round_ps( v3, _MM_ROUND_NEAREST );

        // Convert floats to integers
        __m256i i0 = _mm256_cvtps_epi32( v0 );
        __m256i i1 = _mm256_cvtps_epi32( v1 );
        __m256i i2 = _mm256_cvtps_epi32( v2 );
        __m256i i3 = _mm256_cvtps_epi32( v3 );

#if defined(__AVX2__)
        // Convert int32 to int16
        i0 = _mm256_packs_epi32( i0, i1 );	// 0, 1, 2, 3,  8, 9, 10, 11,  4, 5, 6, 7, 12, 13, 14, 15
        i2 = _mm256_packs_epi32( i2, i3 );	// 16, 17, 18, 19,  24, 25, 26, 27,  20, 21, 22, 23, 28, 29, 30, 31
                                            // Convert int16 to int8
        i0 = _mm256_packs_epi16( i0, i2 );	// 0, 1, 2, 3,  8, 9, 10, 11,  16, 17, 18, 19,  24, 25, 26, 27,  4, 5, 6, 7, 12, 13, 14, 15, 20, 21, 22, 23, 28, 29, 30, 31

        // We got our precious signed bytes, but the order is now wrong
        // These AVX2 pack instructions process 16-byte pieces independently
        // The following instruction is fixing the order
        const __m256i perm = _mm256_setr_epi32( 0, 4, 1, 5, 2, 6, 3, 7 );
        i0 = _mm256_permutevar8x32_epi32( i0, perm );

        _mm256_storeu_si256((__m256i *)y[i].qs, i0);
#else
        // Since we don't have in AVX some necessary functions,
        // we split the registers in half and call AVX2 analogs from SSE
        __m128i ni0 = _mm256_castsi256_si128( i0 );
        __m128i ni1 = _mm256_extractf128_si256( i0, 1);
        __m128i ni2 = _mm256_castsi256_si128( i1 );
        __m128i ni3 = _mm256_extractf128_si256( i1, 1);
        __m128i ni4 = _mm256_castsi256_si128( i2 );
        __m128i ni5 = _mm256_extractf128_si256( i2, 1);
        __m128i ni6 = _mm256_castsi256_si128( i3 );
        __m128i ni7 = _mm256_extractf128_si256( i3, 1);

        // Convert int32 to int16
        ni0 = _mm_packs_epi32( ni0, ni1 );
        ni2 = _mm_packs_epi32( ni2, ni3 );
        ni4 = _mm_packs_epi32( ni4, ni5 );
        ni6 = _mm_packs_epi32( ni6, ni7 );
        // Convert int16 to int8
        ni0 = _mm_packs_epi16( ni0, ni2 );
        ni4 = _mm_packs_epi16( ni4, ni6 );

        _mm_storeu_si128((__m128i *)(y[i].qs +  0), ni0);
        _mm_storeu_si128((__m128i *)(y[i].qs + 16), ni4);
#endif
    }
#elif defined(__riscv_v_intrinsic)

    size_t vl = __riscv_vsetvl_e32m4(QK8_0);

    for (int i = 0; i < nb; i++) {
        // load elements
        vfloat32m4_t v_x   = __riscv_vle32_v_f32m4(x+i*QK8_0, vl);

        vfloat32m4_t vfabs = __riscv_vfabs_v_f32m4(v_x, vl);
        vfloat32m1_t tmp   = __riscv_vfmv_v_f_f32m1(0.0f, vl);
        vfloat32m1_t vmax  = __riscv_vfredmax_vs_f32m4_f32m1(vfabs, tmp, vl);
        float amax = __riscv_vfmv_f_s_f32m1_f32(vmax);

        const float d = amax / ((1 << 7) - 1);
        const float id = d ? 1.0f/d : 0.0f;

        y[i].d = LM_GGML_FP32_TO_FP16(d);

        vfloat32m4_t x0 = __riscv_vfmul_vf_f32m4(v_x, id, vl);

        // convert to integer
        vint16m2_t   vi = __riscv_vfncvt_x_f_w_i16m2(x0, vl);
        vint8m1_t    vs = __riscv_vncvt_x_x_w_i8m1(vi, vl);

        // store result
        __riscv_vse8_v_i8m1(y[i].qs , vs, vl);
    }
#else
    // scalar
    quantize_row_q8_0_reference(x, y, k);
#endif
}

// reference implementation for deterministic creation of model files
static void quantize_row_q8_1_reference(const float * restrict x, block_q8_1 * restrict y, int k) {
    assert(QK8_1 == 32);
    assert(k % QK8_1 == 0);
    const int nb = k / QK8_1;

    for (int i = 0; i < nb; i++) {
        float amax = 0.0f; // absolute max

        for (int j = 0; j < QK8_1; j++) {
            const float v = x[i*QK8_1 + j];
            amax = MAX(amax, fabsf(v));
        }

        const float d = amax / ((1 << 7) - 1);
        const float id = d ? 1.0f/d : 0.0f;

        y[i].d = d;

        int sum = 0;

        for (int j = 0; j < QK8_1/2; ++j) {
            const float v0 = x[i*QK8_1           + j]*id;
            const float v1 = x[i*QK8_1 + QK8_1/2 + j]*id;

            y[i].qs[          j] = roundf(v0);
            y[i].qs[QK8_1/2 + j] = roundf(v1);

            sum += y[i].qs[          j];
            sum += y[i].qs[QK8_1/2 + j];
        }

        y[i].s = sum*d;
    }
}

static void quantize_row_q8_1(const float * restrict x, void * restrict vy, int k) {
    assert(k % QK8_1 == 0);
    const int nb = k / QK8_1;

    block_q8_1 * restrict y = vy;

#if defined(__ARM_NEON)
    for (int i = 0; i < nb; i++) {
        float32x4_t srcv [8];
        float32x4_t asrcv[8];
        float32x4_t amaxv[8];

        for (int j = 0; j < 8; j++) srcv[j]  = vld1q_f32(x + i*32 + 4*j);
        for (int j = 0; j < 8; j++) asrcv[j] = vabsq_f32(srcv[j]);

        for (int j = 0; j < 4; j++) amaxv[2*j] = vmaxq_f32(asrcv[2*j], asrcv[2*j+1]);
        for (int j = 0; j < 2; j++) amaxv[4*j] = vmaxq_f32(amaxv[4*j], amaxv[4*j+2]);
        for (int j = 0; j < 1; j++) amaxv[8*j] = vmaxq_f32(amaxv[8*j], amaxv[8*j+4]);

        const float amax = vmaxvq_f32(amaxv[0]);

        const float d = amax / ((1 << 7) - 1);
        const float id = d ? 1.0f/d : 0.0f;

        y[i].d = d;

        int32x4_t accv = vdupq_n_s32(0);

        for (int j = 0; j < 8; j++) {
            const float32x4_t v  = vmulq_n_f32(srcv[j], id);
            const int32x4_t   vi = vcvtnq_s32_f32(v);

            y[i].qs[4*j + 0] = vgetq_lane_s32(vi, 0);
            y[i].qs[4*j + 1] = vgetq_lane_s32(vi, 1);
            y[i].qs[4*j + 2] = vgetq_lane_s32(vi, 2);
            y[i].qs[4*j + 3] = vgetq_lane_s32(vi, 3);

            accv = vaddq_s32(accv, vi);
        }

        y[i].s = d * vaddvq_s32(accv);
    }
#elif defined(__wasm_simd128__)
    for (int i = 0; i < nb; i++) {
        v128_t srcv [8];
        v128_t asrcv[8];
        v128_t amaxv[8];

        for (int j = 0; j < 8; j++) srcv[j]  = wasm_v128_load(x + i*32 + 4*j);
        for (int j = 0; j < 8; j++) asrcv[j] = wasm_f32x4_abs(srcv[j]);

        for (int j = 0; j < 4; j++) amaxv[2*j] = wasm_f32x4_max(asrcv[2*j], asrcv[2*j+1]);
        for (int j = 0; j < 2; j++) amaxv[4*j] = wasm_f32x4_max(amaxv[4*j], amaxv[4*j+2]);
        for (int j = 0; j < 1; j++) amaxv[8*j] = wasm_f32x4_max(amaxv[8*j], amaxv[8*j+4]);

        const float amax = MAX(MAX(wasm_f32x4_extract_lane(amaxv[0], 0),
                                   wasm_f32x4_extract_lane(amaxv[0], 1)),
                               MAX(wasm_f32x4_extract_lane(amaxv[0], 2),
                                   wasm_f32x4_extract_lane(amaxv[0], 3)));

        const float d = amax / ((1 << 7) - 1);
        const float id = d ? 1.0f/d : 0.0f;

        y[i].d = d;

        v128_t accv = wasm_i32x4_splat(0);

        for (int j = 0; j < 8; j++) {
            const v128_t v  = wasm_f32x4_mul(srcv[j], wasm_f32x4_splat(id));
            const v128_t vi = wasm_i32x4_trunc_sat_f32x4(v);

            y[i].qs[4*j + 0] = wasm_i32x4_extract_lane(vi, 0);
            y[i].qs[4*j + 1] = wasm_i32x4_extract_lane(vi, 1);
            y[i].qs[4*j + 2] = wasm_i32x4_extract_lane(vi, 2);
            y[i].qs[4*j + 3] = wasm_i32x4_extract_lane(vi, 3);

            accv = wasm_i32x4_add(accv, vi);
        }

        y[i].s = d * (wasm_i32x4_extract_lane(accv, 0) +
                      wasm_i32x4_extract_lane(accv, 1) +
                      wasm_i32x4_extract_lane(accv, 2) +
                      wasm_i32x4_extract_lane(accv, 3));
    }
#elif defined(__AVX2__) || defined(__AVX__)
    for (int i = 0; i < nb; i++) {
        // Load elements into 4 AVX vectors
        __m256 v0 = _mm256_loadu_ps( x );
        __m256 v1 = _mm256_loadu_ps( x + 8 );
        __m256 v2 = _mm256_loadu_ps( x + 16 );
        __m256 v3 = _mm256_loadu_ps( x + 24 );
        x += 32;

        // Compute max(abs(e)) for the block
        const __m256 signBit = _mm256_set1_ps( -0.0f );
        __m256 maxAbs = _mm256_andnot_ps( signBit, v0 );
        maxAbs = _mm256_max_ps( maxAbs, _mm256_andnot_ps( signBit, v1 ) );
        maxAbs = _mm256_max_ps( maxAbs, _mm256_andnot_ps( signBit, v2 ) );
        maxAbs = _mm256_max_ps( maxAbs, _mm256_andnot_ps( signBit, v3 ) );

        __m128 max4 = _mm_max_ps( _mm256_extractf128_ps( maxAbs, 1 ), _mm256_castps256_ps128( maxAbs ) );
        max4 = _mm_max_ps( max4, _mm_movehl_ps( max4, max4 ) );
        max4 = _mm_max_ss( max4, _mm_movehdup_ps( max4 ) );
        const float maxScalar = _mm_cvtss_f32( max4 );

        // Quantize these floats
        const float d = maxScalar / 127.f;
        y[i].d = d;
        const float id = ( maxScalar != 0.0f ) ? 127.f / maxScalar : 0.0f;
        const __m256 mul = _mm256_set1_ps( id );

        // Apply the multiplier
        v0 = _mm256_mul_ps( v0, mul );
        v1 = _mm256_mul_ps( v1, mul );
        v2 = _mm256_mul_ps( v2, mul );
        v3 = _mm256_mul_ps( v3, mul );

        // Round to nearest integer
        v0 = _mm256_round_ps( v0, _MM_ROUND_NEAREST );
        v1 = _mm256_round_ps( v1, _MM_ROUND_NEAREST );
        v2 = _mm256_round_ps( v2, _MM_ROUND_NEAREST );
        v3 = _mm256_round_ps( v3, _MM_ROUND_NEAREST );

        // Convert floats to integers
        __m256i i0 = _mm256_cvtps_epi32( v0 );
        __m256i i1 = _mm256_cvtps_epi32( v1 );
        __m256i i2 = _mm256_cvtps_epi32( v2 );
        __m256i i3 = _mm256_cvtps_epi32( v3 );

#if defined(__AVX2__)
        // Compute the sum of the quants and set y[i].s
        y[i].s = d * hsum_i32_8(_mm256_add_epi32(_mm256_add_epi32(i0, i1), _mm256_add_epi32(i2, i3)));

        // Convert int32 to int16
        i0 = _mm256_packs_epi32( i0, i1 );	// 0, 1, 2, 3,  8, 9, 10, 11,  4, 5, 6, 7, 12, 13, 14, 15
        i2 = _mm256_packs_epi32( i2, i3 );	// 16, 17, 18, 19,  24, 25, 26, 27,  20, 21, 22, 23, 28, 29, 30, 31
                                            // Convert int16 to int8
        i0 = _mm256_packs_epi16( i0, i2 );	// 0, 1, 2, 3,  8, 9, 10, 11,  16, 17, 18, 19,  24, 25, 26, 27,  4, 5, 6, 7, 12, 13, 14, 15, 20, 21, 22, 23, 28, 29, 30, 31

        // We got our precious signed bytes, but the order is now wrong
        // These AVX2 pack instructions process 16-byte pieces independently
        // The following instruction is fixing the order
        const __m256i perm = _mm256_setr_epi32( 0, 4, 1, 5, 2, 6, 3, 7 );
        i0 = _mm256_permutevar8x32_epi32( i0, perm );

        _mm256_storeu_si256((__m256i *)y[i].qs, i0);
#else
        // Since we don't have in AVX some necessary functions,
        // we split the registers in half and call AVX2 analogs from SSE
        __m128i ni0 = _mm256_castsi256_si128( i0 );
        __m128i ni1 = _mm256_extractf128_si256( i0, 1);
        __m128i ni2 = _mm256_castsi256_si128( i1 );
        __m128i ni3 = _mm256_extractf128_si256( i1, 1);
        __m128i ni4 = _mm256_castsi256_si128( i2 );
        __m128i ni5 = _mm256_extractf128_si256( i2, 1);
        __m128i ni6 = _mm256_castsi256_si128( i3 );
        __m128i ni7 = _mm256_extractf128_si256( i3, 1);

        // Compute the sum of the quants and set y[i].s
        const __m128i s0 = _mm_add_epi32(_mm_add_epi32(ni0, ni1), _mm_add_epi32(ni2, ni3));
        const __m128i s1 = _mm_add_epi32(_mm_add_epi32(ni4, ni5), _mm_add_epi32(ni6, ni7));
        y[i].s = d * hsum_i32_4(_mm_add_epi32(s0, s1));

        // Convert int32 to int16
        ni0 = _mm_packs_epi32( ni0, ni1 );
        ni2 = _mm_packs_epi32( ni2, ni3 );
        ni4 = _mm_packs_epi32( ni4, ni5 );
        ni6 = _mm_packs_epi32( ni6, ni7 );
        // Convert int16 to int8
        ni0 = _mm_packs_epi16( ni0, ni2 );
        ni4 = _mm_packs_epi16( ni4, ni6 );

        _mm_storeu_si128((__m128i *)(y[i].qs +  0), ni0);
        _mm_storeu_si128((__m128i *)(y[i].qs + 16), ni4);
#endif
    }
#elif defined(__riscv_v_intrinsic)

    size_t vl = __riscv_vsetvl_e32m4(QK8_1);

    for (int i = 0; i < nb; i++) {
        // load elements
        vfloat32m4_t v_x   = __riscv_vle32_v_f32m4(x+i*QK8_1, vl);

        vfloat32m4_t vfabs = __riscv_vfabs_v_f32m4(v_x, vl);
        vfloat32m1_t tmp   = __riscv_vfmv_v_f_f32m1(0.0, vl);
        vfloat32m1_t vmax  = __riscv_vfredmax_vs_f32m4_f32m1(vfabs, tmp, vl);
        float amax = __riscv_vfmv_f_s_f32m1_f32(vmax);

        const float d  = amax / ((1 << 7) - 1);
        const float id = d ? 1.0f/d : 0.0f;

        y[i].d = d;

        vfloat32m4_t x0 = __riscv_vfmul_vf_f32m4(v_x, id, vl);

        // convert to integer
        vint16m2_t   vi = __riscv_vfncvt_x_f_w_i16m2(x0, vl);
        vint8m1_t    vs = __riscv_vncvt_x_x_w_i8m1(vi, vl);

        // store result
        __riscv_vse8_v_i8m1(y[i].qs , vs, vl);

        // compute sum for y[i].s
        vint16m1_t tmp2 = __riscv_vmv_v_x_i16m1(0, vl);
        vint16m1_t vwrs = __riscv_vwredsum_vs_i8m1_i16m1(vs, tmp2, vl);

        // set y[i].s
        int sum = __riscv_vmv_x_s_i16m1_i16(vwrs);
        y[i].s = sum*d;
    }
#else
    // scalar
    quantize_row_q8_1_reference(x, y, k);
#endif
}

static void dequantize_row_q4_0(const block_q4_0 * restrict x, float * restrict y, int k) {
    static const int qk = QK4_0;

    assert(k % qk == 0);

    const int nb = k / qk;

    for (int i = 0; i < nb; i++) {
        const float d = LM_GGML_FP16_TO_FP32(x[i].d);

        for (int j = 0; j < qk/2; ++j) {
            const int x0 = (x[i].qs[j] & 0x0F) - 8;
            const int x1 = (x[i].qs[j] >>   4) - 8;

            y[i*qk + j + 0   ] = x0*d;
            y[i*qk + j + qk/2] = x1*d;
        }
    }
}

static void dequantize_row_q4_1(const block_q4_1 * restrict x, float * restrict y, int k) {
    static const int qk = QK4_1;

    assert(k % qk == 0);

    const int nb = k / qk;

    for (int i = 0; i < nb; i++) {
        const float d = LM_GGML_FP16_TO_FP32(x[i].d);
        const float m = LM_GGML_FP16_TO_FP32(x[i].m);

        for (int j = 0; j < qk/2; ++j) {
            const int x0 = (x[i].qs[j] & 0x0F);
            const int x1 = (x[i].qs[j] >>   4);

            y[i*qk + j + 0   ] = x0*d + m;
            y[i*qk + j + qk/2] = x1*d + m;
        }
    }
}

static void dequantize_row_q5_0(const block_q5_0 * restrict x, float * restrict y, int k) {
    static const int qk = QK5_0;

    assert(k % qk == 0);

    const int nb = k / qk;

    for (int i = 0; i < nb; i++) {
        const float d = LM_GGML_FP16_TO_FP32(x[i].d);

        uint32_t qh;
        memcpy(&qh, x[i].qh, sizeof(qh));

        for (int j = 0; j < qk/2; ++j) {
            const uint8_t xh_0 = ((qh >> (j +  0)) << 4) & 0x10;
            const uint8_t xh_1 = ((qh >> (j + 12))     ) & 0x10;

            const int32_t x0 = ((x[i].qs[j] & 0x0F) | xh_0) - 16;
            const int32_t x1 = ((x[i].qs[j] >>   4) | xh_1) - 16;

            y[i*qk + j + 0   ] = x0*d;
            y[i*qk + j + qk/2] = x1*d;
        }
    }
}

static void dequantize_row_q5_1(const block_q5_1 * restrict x, float * restrict y, int k) {
    static const int qk = QK5_1;

    assert(k % qk == 0);

    const int nb = k / qk;

    for (int i = 0; i < nb; i++) {
        const float d = LM_GGML_FP16_TO_FP32(x[i].d);
        const float m = LM_GGML_FP16_TO_FP32(x[i].m);

        uint32_t qh;
        memcpy(&qh, x[i].qh, sizeof(qh));

        for (int j = 0; j < qk/2; ++j) {
            const uint8_t xh_0 = ((qh >> (j +  0)) << 4) & 0x10;
            const uint8_t xh_1 = ((qh >> (j + 12))     ) & 0x10;

            const int x0 = (x[i].qs[j] & 0x0F) | xh_0;
            const int x1 = (x[i].qs[j] >>   4) | xh_1;

            y[i*qk + j + 0   ] = x0*d + m;
            y[i*qk + j + qk/2] = x1*d + m;
        }
    }
}

static void dequantize_row_q8_0(const void * restrict vx, float * restrict y, int k) {
    static const int qk = QK8_0;

    assert(k % qk == 0);

    const int nb = k / qk;

    const block_q8_0 * restrict x = vx;

    for (int i = 0; i < nb; i++) {
        const float d = LM_GGML_FP16_TO_FP32(x[i].d);

        for (int j = 0; j < qk; ++j) {
            y[i*qk + j] = x[i].qs[j]*d;
        }
    }
}

static void lm_ggml_vec_dot_f32(const int n, float * restrict s, const float * restrict x, const float * restrict y);
static void lm_ggml_vec_dot_f16(const int n, float * restrict s, lm_ggml_fp16_t * restrict x, lm_ggml_fp16_t * restrict y);
static void lm_ggml_vec_dot_q4_0_q8_0(const int n, float * restrict s, const void * restrict vx, const void * restrict vy);
static void lm_ggml_vec_dot_q4_1_q8_1(const int n, float * restrict s, const void * restrict vx, const void * restrict vy);
static void lm_ggml_vec_dot_q5_0_q8_0(const int n, float * restrict s, const void * restrict vx, const void * restrict vy);
static void lm_ggml_vec_dot_q5_1_q8_1(const int n, float * restrict s, const void * restrict vx, const void * restrict vy);
static void lm_ggml_vec_dot_q8_0_q8_0(const int n, float * restrict s, const void * restrict vx, const void * restrict vy);

static const lm_ggml_type_traits_t type_traits[LM_GGML_TYPE_COUNT] = {
    [LM_GGML_TYPE_I8] = {
        .type_name                = "i8",
        .blck_size                = 1,
        .type_size                = sizeof(int8_t),
        .is_quantized             = false,
    },
    [LM_GGML_TYPE_I16] = {
        .type_name                = "i16",
        .blck_size                = 1,
        .type_size                = sizeof(int16_t),
        .is_quantized             = false,
    },
    [LM_GGML_TYPE_I32] = {
        .type_name                = "i32",
        .blck_size                = 1,
        .type_size                = sizeof(int32_t),
        .is_quantized             = false,
    },
    [LM_GGML_TYPE_F32] = {
        .type_name                = "f32",
        .blck_size                = 1,
        .type_size                = sizeof(float),
        .is_quantized             = false,
        .vec_dot                  = (lm_ggml_vec_dot_t) lm_ggml_vec_dot_f32,
        .vec_dot_type             = LM_GGML_TYPE_F32,
    },
    [LM_GGML_TYPE_F16] = {
        .type_name                = "f16",
        .blck_size                = 1,
        .type_size                = sizeof(lm_ggml_fp16_t),
        .is_quantized             = false,
        .to_float                 = (lm_ggml_to_float_t) lm_ggml_fp16_to_fp32_row,
        .from_float               = (lm_ggml_from_float_t) lm_ggml_fp32_to_fp16_row,
        .from_float_reference     = (lm_ggml_from_float_t) lm_ggml_fp32_to_fp16_row,
        .vec_dot                  = (lm_ggml_vec_dot_t) lm_ggml_vec_dot_f16,
        .vec_dot_type             = LM_GGML_TYPE_F16,
    },
    [LM_GGML_TYPE_Q4_0] = {
        .type_name                = "q4_0",
        .blck_size                = QK4_0,
        .type_size                = sizeof(block_q4_0),
        .is_quantized             = true,
        .to_float                 = (lm_ggml_to_float_t) dequantize_row_q4_0,
        .from_float               = quantize_row_q4_0,
        .from_float_reference     = (lm_ggml_from_float_t) quantize_row_q4_0_reference,
        .vec_dot                  = lm_ggml_vec_dot_q4_0_q8_0,
        .vec_dot_type             = LM_GGML_TYPE_Q8_0,
    },
    [LM_GGML_TYPE_Q4_1] = {
        .type_name                = "q4_1",
        .blck_size                = QK4_1,
        .type_size                = sizeof(block_q4_1),
        .is_quantized             = true,
        .to_float                 = (lm_ggml_to_float_t) dequantize_row_q4_1,
        .from_float               = quantize_row_q4_1,
        .from_float_reference     = (lm_ggml_from_float_t) quantize_row_q4_1_reference,
        .vec_dot                  = lm_ggml_vec_dot_q4_1_q8_1,
        .vec_dot_type             = LM_GGML_TYPE_Q8_1,
    },
    [LM_GGML_TYPE_Q5_0] = {
        .type_name                = "q5_0",
        .blck_size                = QK5_0,
        .type_size                = sizeof(block_q5_0),
        .is_quantized             = true,
        .to_float                 = (lm_ggml_to_float_t) dequantize_row_q5_0,
        .from_float               = quantize_row_q5_0,
        .from_float_reference     = (lm_ggml_from_float_t) quantize_row_q5_0_reference,
        .vec_dot                  = lm_ggml_vec_dot_q5_0_q8_0,
        .vec_dot_type             = LM_GGML_TYPE_Q8_0,
    },
    [LM_GGML_TYPE_Q5_1] = {
        .type_name                = "q5_1",
        .blck_size                = QK5_1,
        .type_size                = sizeof(block_q5_1),
        .is_quantized             = true,
        .to_float                 = (lm_ggml_to_float_t) dequantize_row_q5_1,
        .from_float               = quantize_row_q5_1,
        .from_float_reference     = (lm_ggml_from_float_t) quantize_row_q5_1_reference,
        .vec_dot                  = lm_ggml_vec_dot_q5_1_q8_1,
        .vec_dot_type             = LM_GGML_TYPE_Q8_1,
    },
    [LM_GGML_TYPE_Q8_0] = {
        .type_name                = "q8_0",
        .blck_size                = QK8_0,
        .type_size                = sizeof(block_q8_0),
        .is_quantized             = true,
        .to_float                 = dequantize_row_q8_0,
        .from_float               = quantize_row_q8_0,
        .from_float_reference     = (lm_ggml_from_float_t) quantize_row_q8_0_reference,
        .vec_dot                  = lm_ggml_vec_dot_q8_0_q8_0,
        .vec_dot_type             = LM_GGML_TYPE_Q8_0,
    },
    [LM_GGML_TYPE_Q8_1] = {
        .type_name                = "q8_1",
        .blck_size                = QK8_1,
        .type_size                = sizeof(block_q8_1),
        .is_quantized             = true,
        .from_float               = quantize_row_q8_1,
        .from_float_reference     = (lm_ggml_from_float_t) quantize_row_q8_1_reference,
        .vec_dot_type             = LM_GGML_TYPE_Q8_1,
    },
#ifdef LM_GGML_USE_K_QUANTS
    [LM_GGML_TYPE_Q2_K] = {
        .type_name                = "q2_K",
        .blck_size                = QK_K,
        .type_size                = sizeof(block_q2_K),
        .is_quantized             = true,
        .to_float                 = (lm_ggml_to_float_t) dequantize_row_q2_K,
        .from_float               = quantize_row_q2_K,
        .from_float_reference     = (lm_ggml_from_float_t) quantize_row_q2_K_reference,
        .vec_dot                  = lm_ggml_vec_dot_q2_K_q8_K,
        .vec_dot_type             = LM_GGML_TYPE_Q8_K,
    },
    [LM_GGML_TYPE_Q3_K] = {
        .type_name                = "q3_K",
        .blck_size                = QK_K,
        .type_size                = sizeof(block_q3_K),
        .is_quantized             = true,
        .to_float                 = (lm_ggml_to_float_t) dequantize_row_q3_K,
        .from_float               = quantize_row_q3_K,
        .from_float_reference     = (lm_ggml_from_float_t) quantize_row_q3_K_reference,
        .vec_dot                  = lm_ggml_vec_dot_q3_K_q8_K,
        .vec_dot_type             = LM_GGML_TYPE_Q8_K,
    },
    [LM_GGML_TYPE_Q4_K] = {
        .type_name                = "q4_K",
        .blck_size                = QK_K,
        .type_size                = sizeof(block_q4_K),
        .is_quantized             = true,
        .to_float                 = (lm_ggml_to_float_t) dequantize_row_q4_K,
        .from_float               = quantize_row_q4_K,
        .from_float_reference     = (lm_ggml_from_float_t) quantize_row_q4_K_reference,
        .vec_dot                  = lm_ggml_vec_dot_q4_K_q8_K,
        .vec_dot_type             = LM_GGML_TYPE_Q8_K,
    },
    [LM_GGML_TYPE_Q5_K] = {
        .type_name                = "q5_K",
        .blck_size                = QK_K,
        .type_size                = sizeof(block_q5_K),
        .is_quantized             = true,
        .to_float                 = (lm_ggml_to_float_t) dequantize_row_q5_K,
        .from_float               = quantize_row_q5_K,
        .from_float_reference     = (lm_ggml_from_float_t) quantize_row_q5_K_reference,
        .vec_dot                  = lm_ggml_vec_dot_q5_K_q8_K,
        .vec_dot_type             = LM_GGML_TYPE_Q8_K,
    },
    [LM_GGML_TYPE_Q6_K] = {
        .type_name                = "q6_K",
        .blck_size                = QK_K,
        .type_size                = sizeof(block_q6_K),
        .is_quantized             = true,
        .to_float                 = (lm_ggml_to_float_t) dequantize_row_q6_K,
        .from_float               = quantize_row_q6_K,
        .from_float_reference     = (lm_ggml_from_float_t) quantize_row_q6_K_reference,
        .vec_dot                  = lm_ggml_vec_dot_q6_K_q8_K,
        .vec_dot_type             = LM_GGML_TYPE_Q8_K,
    },
    [LM_GGML_TYPE_Q8_K] = {
        .type_name                = "q8_K",
        .blck_size                = QK_K,
        .type_size                = sizeof(block_q8_K),
        .is_quantized             = true,
        .from_float               = quantize_row_q8_K,
    }
#endif
};

// For internal test use
lm_ggml_type_traits_t lm_ggml_internal_get_type_traits(enum lm_ggml_type type) {
    LM_GGML_ASSERT(type < LM_GGML_TYPE_COUNT);
    return type_traits[type];
}

//
// simd mappings
//

// we define a common set of C macros which map to specific intrinsics based on the current architecture
// we then implement the fundamental computation operations below using only these macros
// adding support for new architectures requires to define the corresponding SIMD macros
//
// LM_GGML_F32_STEP / LM_GGML_F16_STEP
//   number of elements to process in a single step
//
// LM_GGML_F32_EPR / LM_GGML_F16_EPR
//   number of elements to fit in a single register
//

#if defined(__ARM_NEON) && defined(__ARM_FEATURE_FMA)

#define LM_GGML_SIMD

// F32 NEON

#define LM_GGML_F32_STEP 16
#define LM_GGML_F32_EPR  4

#define LM_GGML_F32x4              float32x4_t
#define LM_GGML_F32x4_ZERO         vdupq_n_f32(0.0f)
#define LM_GGML_F32x4_SET1(x)      vdupq_n_f32(x)
#define LM_GGML_F32x4_LOAD         vld1q_f32
#define LM_GGML_F32x4_STORE        vst1q_f32
#define LM_GGML_F32x4_FMA(a, b, c) vfmaq_f32(a, b, c)
#define LM_GGML_F32x4_ADD          vaddq_f32
#define LM_GGML_F32x4_MUL          vmulq_f32
#define LM_GGML_F32x4_REDUCE_ONE(x) vaddvq_f32(x)
#define LM_GGML_F32x4_REDUCE(res, x)              \
{                                              \
    int offset = LM_GGML_F32_ARR >> 1;            \
    for (int i = 0; i < offset; ++i) {         \
        x[i] = vaddq_f32(x[i], x[offset+i]);   \
    }                                          \
    offset >>= 1;                              \
    for (int i = 0; i < offset; ++i) {         \
        x[i] = vaddq_f32(x[i], x[offset+i]);   \
    }                                          \
    offset >>= 1;                              \
    for (int i = 0; i < offset; ++i) {         \
        x[i] = vaddq_f32(x[i], x[offset+i]);   \
    }                                          \
    res = LM_GGML_F32x4_REDUCE_ONE(x[0]);         \
}

#define LM_GGML_F32_VEC        LM_GGML_F32x4
#define LM_GGML_F32_VEC_ZERO   LM_GGML_F32x4_ZERO
#define LM_GGML_F32_VEC_SET1   LM_GGML_F32x4_SET1
#define LM_GGML_F32_VEC_LOAD   LM_GGML_F32x4_LOAD
#define LM_GGML_F32_VEC_STORE  LM_GGML_F32x4_STORE
#define LM_GGML_F32_VEC_FMA    LM_GGML_F32x4_FMA
#define LM_GGML_F32_VEC_ADD    LM_GGML_F32x4_ADD
#define LM_GGML_F32_VEC_MUL    LM_GGML_F32x4_MUL
#define LM_GGML_F32_VEC_REDUCE LM_GGML_F32x4_REDUCE

// F16 NEON

#if defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC)
    #define LM_GGML_F16_STEP 32
    #define LM_GGML_F16_EPR  8

    #define LM_GGML_F16x8              float16x8_t
    #define LM_GGML_F16x8_ZERO         vdupq_n_f16(0.0f)
    #define LM_GGML_F16x8_SET1(x)      vdupq_n_f16(x)
    #define LM_GGML_F16x8_LOAD         vld1q_f16
    #define LM_GGML_F16x8_STORE        vst1q_f16
    #define LM_GGML_F16x8_FMA(a, b, c) vfmaq_f16(a, b, c)
    #define LM_GGML_F16x8_ADD          vaddq_f16
    #define LM_GGML_F16x8_MUL          vmulq_f16
    #define LM_GGML_F16x8_REDUCE(res, x)                             \
    do {                                                          \
        int offset = LM_GGML_F16_ARR >> 1;                           \
        for (int i = 0; i < offset; ++i) {                        \
            x[i] = vaddq_f16(x[i], x[offset+i]);                  \
        }                                                         \
        offset >>= 1;                                             \
        for (int i = 0; i < offset; ++i) {                        \
            x[i] = vaddq_f16(x[i], x[offset+i]);                  \
        }                                                         \
        offset >>= 1;                                             \
        for (int i = 0; i < offset; ++i) {                        \
            x[i] = vaddq_f16(x[i], x[offset+i]);                  \
        }                                                         \
        const float32x4_t t0 = vcvt_f32_f16(vget_low_f16 (x[0])); \
        const float32x4_t t1 = vcvt_f32_f16(vget_high_f16(x[0])); \
        res = (lm_ggml_float) vaddvq_f32(vaddq_f32(t0, t1));         \
    } while (0)

    #define LM_GGML_F16_VEC                LM_GGML_F16x8
    #define LM_GGML_F16_VEC_ZERO           LM_GGML_F16x8_ZERO
    #define LM_GGML_F16_VEC_SET1           LM_GGML_F16x8_SET1
    #define LM_GGML_F16_VEC_LOAD(p, i)     LM_GGML_F16x8_LOAD(p)
    #define LM_GGML_F16_VEC_STORE(p, r, i) LM_GGML_F16x8_STORE(p, r[i])
    #define LM_GGML_F16_VEC_FMA            LM_GGML_F16x8_FMA
    #define LM_GGML_F16_VEC_ADD            LM_GGML_F16x8_ADD
    #define LM_GGML_F16_VEC_MUL            LM_GGML_F16x8_MUL
    #define LM_GGML_F16_VEC_REDUCE         LM_GGML_F16x8_REDUCE
#else
    // if FP16 vector arithmetic is not supported, we use FP32 instead
    // and take advantage of the vcvt_ functions to convert to/from FP16

    #define LM_GGML_F16_STEP 16
    #define LM_GGML_F16_EPR  4

    #define LM_GGML_F32Cx4              float32x4_t
    #define LM_GGML_F32Cx4_ZERO         vdupq_n_f32(0.0f)
    #define LM_GGML_F32Cx4_SET1(x)      vdupq_n_f32(x)
    #define LM_GGML_F32Cx4_LOAD(x)      vcvt_f32_f16(vld1_f16(x))
    #define LM_GGML_F32Cx4_STORE(x, y)  vst1_f16(x, vcvt_f16_f32(y))
    #define LM_GGML_F32Cx4_FMA(a, b, c) vfmaq_f32(a, b, c)
    #define LM_GGML_F32Cx4_ADD          vaddq_f32
    #define LM_GGML_F32Cx4_MUL          vmulq_f32
    #define LM_GGML_F32Cx4_REDUCE       LM_GGML_F32x4_REDUCE

    #define LM_GGML_F16_VEC                LM_GGML_F32Cx4
    #define LM_GGML_F16_VEC_ZERO           LM_GGML_F32Cx4_ZERO
    #define LM_GGML_F16_VEC_SET1           LM_GGML_F32Cx4_SET1
    #define LM_GGML_F16_VEC_LOAD(p, i)     LM_GGML_F32Cx4_LOAD(p)
    #define LM_GGML_F16_VEC_STORE(p, r, i) LM_GGML_F32Cx4_STORE(p, r[i])
    #define LM_GGML_F16_VEC_FMA            LM_GGML_F32Cx4_FMA
    #define LM_GGML_F16_VEC_ADD            LM_GGML_F32Cx4_ADD
    #define LM_GGML_F16_VEC_MUL            LM_GGML_F32Cx4_MUL
    #define LM_GGML_F16_VEC_REDUCE         LM_GGML_F32Cx4_REDUCE
#endif

#elif defined(__AVX__)

#define LM_GGML_SIMD

// F32 AVX

#define LM_GGML_F32_STEP 32
#define LM_GGML_F32_EPR  8

#define LM_GGML_F32x8         __m256
#define LM_GGML_F32x8_ZERO    _mm256_setzero_ps()
#define LM_GGML_F32x8_SET1(x) _mm256_set1_ps(x)
#define LM_GGML_F32x8_LOAD    _mm256_loadu_ps
#define LM_GGML_F32x8_STORE   _mm256_storeu_ps
#if defined(__FMA__)
    #define LM_GGML_F32x8_FMA(a, b, c) _mm256_fmadd_ps(b, c, a)
#else
    #define LM_GGML_F32x8_FMA(a, b, c) _mm256_add_ps(_mm256_mul_ps(b, c), a)
#endif
#define LM_GGML_F32x8_ADD     _mm256_add_ps
#define LM_GGML_F32x8_MUL     _mm256_mul_ps
#define LM_GGML_F32x8_REDUCE(res, x)                                 \
do {                                                              \
    int offset = LM_GGML_F32_ARR >> 1;                               \
    for (int i = 0; i < offset; ++i) {                            \
        x[i] = _mm256_add_ps(x[i], x[offset+i]);                  \
    }                                                             \
    offset >>= 1;                                                 \
    for (int i = 0; i < offset; ++i) {                            \
        x[i] = _mm256_add_ps(x[i], x[offset+i]);                  \
    }                                                             \
    offset >>= 1;                                                 \
    for (int i = 0; i < offset; ++i) {                            \
        x[i] = _mm256_add_ps(x[i], x[offset+i]);                  \
    }                                                             \
    const __m128 t0 = _mm_add_ps(_mm256_castps256_ps128(x[0]),    \
                                 _mm256_extractf128_ps(x[0], 1)); \
    const __m128 t1 = _mm_hadd_ps(t0, t0);                        \
    res = _mm_cvtss_f32(_mm_hadd_ps(t1, t1));                     \
} while (0)
// TODO: is this optimal ?

#define LM_GGML_F32_VEC        LM_GGML_F32x8
#define LM_GGML_F32_VEC_ZERO   LM_GGML_F32x8_ZERO
#define LM_GGML_F32_VEC_SET1   LM_GGML_F32x8_SET1
#define LM_GGML_F32_VEC_LOAD   LM_GGML_F32x8_LOAD
#define LM_GGML_F32_VEC_STORE  LM_GGML_F32x8_STORE
#define LM_GGML_F32_VEC_FMA    LM_GGML_F32x8_FMA
#define LM_GGML_F32_VEC_ADD    LM_GGML_F32x8_ADD
#define LM_GGML_F32_VEC_MUL    LM_GGML_F32x8_MUL
#define LM_GGML_F32_VEC_REDUCE LM_GGML_F32x8_REDUCE

// F16 AVX

#define LM_GGML_F16_STEP 32
#define LM_GGML_F16_EPR  8

// F16 arithmetic is not supported by AVX, so we use F32 instead

#define LM_GGML_F32Cx8             __m256
#define LM_GGML_F32Cx8_ZERO        _mm256_setzero_ps()
#define LM_GGML_F32Cx8_SET1(x)     _mm256_set1_ps(x)

#if defined(__F16C__)
// the  _mm256_cvt intrinsics require F16C
#define LM_GGML_F32Cx8_LOAD(x)     _mm256_cvtph_ps(_mm_loadu_si128((__m128i *)(x)))
#define LM_GGML_F32Cx8_STORE(x, y) _mm_storeu_si128((__m128i *)(x), _mm256_cvtps_ph(y, 0))
#else
static inline __m256 __avx_f32cx8_load(lm_ggml_fp16_t *x) {
    float tmp[8];

    for (int i = 0; i < 8; i++) {
        tmp[i] = LM_GGML_FP16_TO_FP32(x[i]);
    }

    return _mm256_loadu_ps(tmp);
}
static inline void __avx_f32cx8_store(lm_ggml_fp16_t *x, __m256 y) {
    float arr[8];

    _mm256_storeu_ps(arr, y);

    for (int i = 0; i < 8; i++)
        x[i] = LM_GGML_FP32_TO_FP16(arr[i]);
}
#define LM_GGML_F32Cx8_LOAD(x)     __avx_f32cx8_load(x)
#define LM_GGML_F32Cx8_STORE(x, y) __avx_f32cx8_store(x, y)
#endif

#define LM_GGML_F32Cx8_FMA         LM_GGML_F32x8_FMA
#define LM_GGML_F32Cx8_ADD         _mm256_add_ps
#define LM_GGML_F32Cx8_MUL         _mm256_mul_ps
#define LM_GGML_F32Cx8_REDUCE      LM_GGML_F32x8_REDUCE

#define LM_GGML_F16_VEC                LM_GGML_F32Cx8
#define LM_GGML_F16_VEC_ZERO           LM_GGML_F32Cx8_ZERO
#define LM_GGML_F16_VEC_SET1           LM_GGML_F32Cx8_SET1
#define LM_GGML_F16_VEC_LOAD(p, i)     LM_GGML_F32Cx8_LOAD(p)
#define LM_GGML_F16_VEC_STORE(p, r, i) LM_GGML_F32Cx8_STORE(p, r[i])
#define LM_GGML_F16_VEC_FMA            LM_GGML_F32Cx8_FMA
#define LM_GGML_F16_VEC_ADD            LM_GGML_F32Cx8_ADD
#define LM_GGML_F16_VEC_MUL            LM_GGML_F32Cx8_MUL
#define LM_GGML_F16_VEC_REDUCE         LM_GGML_F32Cx8_REDUCE

#elif defined(__POWER9_VECTOR__)

#define LM_GGML_SIMD

// F32 POWER9

#define LM_GGML_F32_STEP 32
#define LM_GGML_F32_EPR  4

#define LM_GGML_F32x4              vector float
#define LM_GGML_F32x4_ZERO         0.0f
#define LM_GGML_F32x4_SET1         vec_splats
#define LM_GGML_F32x4_LOAD(p)      vec_xl(0, p)
#define LM_GGML_F32x4_STORE(p, r)  vec_xst(r, 0, p)
#define LM_GGML_F32x4_FMA(a, b, c) vec_madd(b, c, a)
#define LM_GGML_F32x4_ADD          vec_add
#define LM_GGML_F32x4_MUL          vec_mul
#define LM_GGML_F32x4_REDUCE(res, x)              \
{                                              \
    int offset = LM_GGML_F32_ARR >> 1;            \
    for (int i = 0; i < offset; ++i) {         \
        x[i] = vec_add(x[i], x[offset+i]);     \
    }                                          \
    offset >>= 1;                              \
    for (int i = 0; i < offset; ++i) {         \
        x[i] = vec_add(x[i], x[offset+i]);     \
    }                                          \
    offset >>= 1;                              \
    for (int i = 0; i < offset; ++i) {         \
        x[i] = vec_add(x[i], x[offset+i]);     \
    }                                          \
    res = vec_extract(x[0], 0) +               \
          vec_extract(x[0], 1) +               \
          vec_extract(x[0], 2) +               \
          vec_extract(x[0], 3);                \
}

#define LM_GGML_F32_VEC        LM_GGML_F32x4
#define LM_GGML_F32_VEC_ZERO   LM_GGML_F32x4_ZERO
#define LM_GGML_F32_VEC_SET1   LM_GGML_F32x4_SET1
#define LM_GGML_F32_VEC_LOAD   LM_GGML_F32x4_LOAD
#define LM_GGML_F32_VEC_STORE  LM_GGML_F32x4_STORE
#define LM_GGML_F32_VEC_FMA    LM_GGML_F32x4_FMA
#define LM_GGML_F32_VEC_ADD    LM_GGML_F32x4_ADD
#define LM_GGML_F32_VEC_MUL    LM_GGML_F32x4_MUL
#define LM_GGML_F32_VEC_REDUCE LM_GGML_F32x4_REDUCE

// F16 POWER9
#define LM_GGML_F16_STEP       LM_GGML_F32_STEP
#define LM_GGML_F16_EPR        LM_GGML_F32_EPR
#define LM_GGML_F16_VEC        LM_GGML_F32x4
#define LM_GGML_F16_VEC_ZERO   LM_GGML_F32x4_ZERO
#define LM_GGML_F16_VEC_SET1   LM_GGML_F32x4_SET1
#define LM_GGML_F16_VEC_FMA    LM_GGML_F32x4_FMA
#define LM_GGML_F16_VEC_REDUCE LM_GGML_F32x4_REDUCE
// Use vec_xl, not vec_ld, in case the load address is not aligned.
#define LM_GGML_F16_VEC_LOAD(p, i) (i & 0x1) ?                   \
  vec_extract_fp32_from_shorth(vec_xl(0, p - LM_GGML_F16_EPR)) : \
  vec_extract_fp32_from_shortl(vec_xl(0, p))
#define LM_GGML_ENDIAN_BYTE(i) ((unsigned char *)&(uint16_t){1})[i]
#define LM_GGML_F16_VEC_STORE(p, r, i)                             \
  if (i & 0x1)                                                  \
    vec_xst(vec_pack_to_short_fp32(r[i - LM_GGML_ENDIAN_BYTE(1)],  \
                                   r[i - LM_GGML_ENDIAN_BYTE(0)]), \
            0, p - LM_GGML_F16_EPR)

#elif defined(__wasm_simd128__)

#define LM_GGML_SIMD

// F32 WASM

#define LM_GGML_F32_STEP 16
#define LM_GGML_F32_EPR  4

#define LM_GGML_F32x4              v128_t
#define LM_GGML_F32x4_ZERO         wasm_f32x4_splat(0.0f)
#define LM_GGML_F32x4_SET1(x)      wasm_f32x4_splat(x)
#define LM_GGML_F32x4_LOAD         wasm_v128_load
#define LM_GGML_F32x4_STORE        wasm_v128_store
#define LM_GGML_F32x4_FMA(a, b, c) wasm_f32x4_add(wasm_f32x4_mul(b, c), a)
#define LM_GGML_F32x4_ADD          wasm_f32x4_add
#define LM_GGML_F32x4_MUL          wasm_f32x4_mul
#define LM_GGML_F32x4_REDUCE(res, x)                  \
{                                                  \
    int offset = LM_GGML_F32_ARR >> 1;                \
    for (int i = 0; i < offset; ++i) {             \
        x[i] = wasm_f32x4_add(x[i], x[offset+i]);  \
    }                                              \
    offset >>= 1;                                  \
    for (int i = 0; i < offset; ++i) {             \
        x[i] = wasm_f32x4_add(x[i], x[offset+i]);  \
    }                                              \
    offset >>= 1;                                  \
    for (int i = 0; i < offset; ++i) {             \
        x[i] = wasm_f32x4_add(x[i], x[offset+i]);  \
    }                                              \
    res = wasm_f32x4_extract_lane(x[0], 0) +       \
          wasm_f32x4_extract_lane(x[0], 1) +       \
          wasm_f32x4_extract_lane(x[0], 2) +       \
          wasm_f32x4_extract_lane(x[0], 3);        \
}

#define LM_GGML_F32_VEC        LM_GGML_F32x4
#define LM_GGML_F32_VEC_ZERO   LM_GGML_F32x4_ZERO
#define LM_GGML_F32_VEC_SET1   LM_GGML_F32x4_SET1
#define LM_GGML_F32_VEC_LOAD   LM_GGML_F32x4_LOAD
#define LM_GGML_F32_VEC_STORE  LM_GGML_F32x4_STORE
#define LM_GGML_F32_VEC_FMA    LM_GGML_F32x4_FMA
#define LM_GGML_F32_VEC_ADD    LM_GGML_F32x4_ADD
#define LM_GGML_F32_VEC_MUL    LM_GGML_F32x4_MUL
#define LM_GGML_F32_VEC_REDUCE LM_GGML_F32x4_REDUCE

// F16 WASM

#define LM_GGML_F16_STEP 16
#define LM_GGML_F16_EPR  4

inline static v128_t __wasm_f16x4_load(const lm_ggml_fp16_t * p) {
    float tmp[4];

    tmp[0] = LM_GGML_FP16_TO_FP32(p[0]);
    tmp[1] = LM_GGML_FP16_TO_FP32(p[1]);
    tmp[2] = LM_GGML_FP16_TO_FP32(p[2]);
    tmp[3] = LM_GGML_FP16_TO_FP32(p[3]);

    return wasm_v128_load(tmp);
}

inline static void __wasm_f16x4_store(lm_ggml_fp16_t * p, v128_t x) {
    float tmp[4];

    wasm_v128_store(tmp, x);

    p[0] = LM_GGML_FP32_TO_FP16(tmp[0]);
    p[1] = LM_GGML_FP32_TO_FP16(tmp[1]);
    p[2] = LM_GGML_FP32_TO_FP16(tmp[2]);
    p[3] = LM_GGML_FP32_TO_FP16(tmp[3]);
}

#define LM_GGML_F16x4             v128_t
#define LM_GGML_F16x4_ZERO        wasm_f32x4_splat(0.0f)
#define LM_GGML_F16x4_SET1(x)     wasm_f32x4_splat(x)
#define LM_GGML_F16x4_LOAD(x)     __wasm_f16x4_load(x)
#define LM_GGML_F16x4_STORE(x, y) __wasm_f16x4_store(x, y)
#define LM_GGML_F16x4_FMA         LM_GGML_F32x4_FMA
#define LM_GGML_F16x4_ADD         wasm_f32x4_add
#define LM_GGML_F16x4_MUL         wasm_f32x4_mul
#define LM_GGML_F16x4_REDUCE(res, x)                  \
{                                                  \
    int offset = LM_GGML_F16_ARR >> 1;                \
    for (int i = 0; i < offset; ++i) {             \
        x[i] = wasm_f32x4_add(x[i], x[offset+i]);  \
    }                                              \
    offset >>= 1;                                  \
    for (int i = 0; i < offset; ++i) {             \
        x[i] = wasm_f32x4_add(x[i], x[offset+i]);  \
    }                                              \
    offset >>= 1;                                  \
    for (int i = 0; i < offset; ++i) {             \
        x[i] = wasm_f32x4_add(x[i], x[offset+i]);  \
    }                                              \
    res = wasm_f32x4_extract_lane(x[0], 0) +       \
          wasm_f32x4_extract_lane(x[0], 1) +       \
          wasm_f32x4_extract_lane(x[0], 2) +       \
          wasm_f32x4_extract_lane(x[0], 3);        \
}

#define LM_GGML_F16_VEC                LM_GGML_F16x4
#define LM_GGML_F16_VEC_ZERO           LM_GGML_F16x4_ZERO
#define LM_GGML_F16_VEC_SET1           LM_GGML_F16x4_SET1
#define LM_GGML_F16_VEC_LOAD(p, i)     LM_GGML_F16x4_LOAD(p)
#define LM_GGML_F16_VEC_STORE(p, r, i) LM_GGML_F16x4_STORE(p, r[i])
#define LM_GGML_F16_VEC_FMA            LM_GGML_F16x4_FMA
#define LM_GGML_F16_VEC_ADD            LM_GGML_F16x4_ADD
#define LM_GGML_F16_VEC_MUL            LM_GGML_F16x4_MUL
#define LM_GGML_F16_VEC_REDUCE         LM_GGML_F16x4_REDUCE

#elif defined(__SSE3__)

#define LM_GGML_SIMD

// F32 SSE

#define LM_GGML_F32_STEP 32
#define LM_GGML_F32_EPR  4

#define LM_GGML_F32x4         __m128
#define LM_GGML_F32x4_ZERO    _mm_setzero_ps()
#define LM_GGML_F32x4_SET1(x) _mm_set1_ps(x)
#define LM_GGML_F32x4_LOAD    _mm_loadu_ps
#define LM_GGML_F32x4_STORE   _mm_storeu_ps
#if defined(__FMA__)
    // TODO: Does this work?
    #define LM_GGML_F32x4_FMA(a, b, c) _mm_fmadd_ps(b, c, a)
#else
    #define LM_GGML_F32x4_FMA(a, b, c) _mm_add_ps(_mm_mul_ps(b, c), a)
#endif
#define LM_GGML_F32x4_ADD     _mm_add_ps
#define LM_GGML_F32x4_MUL     _mm_mul_ps
#define LM_GGML_F32x4_REDUCE(res, x)                                 \
{                                                                 \
    int offset = LM_GGML_F32_ARR >> 1;                               \
    for (int i = 0; i < offset; ++i) {                            \
        x[i] = _mm_add_ps(x[i], x[offset+i]);                     \
    }                                                             \
    offset >>= 1;                                                 \
    for (int i = 0; i < offset; ++i) {                            \
        x[i] = _mm_add_ps(x[i], x[offset+i]);                     \
    }                                                             \
    offset >>= 1;                                                 \
    for (int i = 0; i < offset; ++i) {                            \
        x[i] = _mm_add_ps(x[i], x[offset+i]);                     \
    }                                                             \
    const __m128 t0 = _mm_hadd_ps(x[0], x[0]);                    \
    res = _mm_cvtss_f32(_mm_hadd_ps(t0, t0));                     \
}
// TODO: is this optimal ?

#define LM_GGML_F32_VEC        LM_GGML_F32x4
#define LM_GGML_F32_VEC_ZERO   LM_GGML_F32x4_ZERO
#define LM_GGML_F32_VEC_SET1   LM_GGML_F32x4_SET1
#define LM_GGML_F32_VEC_LOAD   LM_GGML_F32x4_LOAD
#define LM_GGML_F32_VEC_STORE  LM_GGML_F32x4_STORE
#define LM_GGML_F32_VEC_FMA    LM_GGML_F32x4_FMA
#define LM_GGML_F32_VEC_ADD    LM_GGML_F32x4_ADD
#define LM_GGML_F32_VEC_MUL    LM_GGML_F32x4_MUL
#define LM_GGML_F32_VEC_REDUCE LM_GGML_F32x4_REDUCE

// F16 SSE

#define LM_GGML_F16_STEP 32
#define LM_GGML_F16_EPR  4

static inline __m128 __sse_f16x4_load(lm_ggml_fp16_t *x) {
    float tmp[4];

    tmp[0] = LM_GGML_FP16_TO_FP32(x[0]);
    tmp[1] = LM_GGML_FP16_TO_FP32(x[1]);
    tmp[2] = LM_GGML_FP16_TO_FP32(x[2]);
    tmp[3] = LM_GGML_FP16_TO_FP32(x[3]);

    return _mm_loadu_ps(tmp);
}

static inline void __sse_f16x4_store(lm_ggml_fp16_t *x, __m128 y) {
    float arr[4];

    _mm_storeu_ps(arr, y);

    x[0] = LM_GGML_FP32_TO_FP16(arr[0]);
    x[1] = LM_GGML_FP32_TO_FP16(arr[1]);
    x[2] = LM_GGML_FP32_TO_FP16(arr[2]);
    x[3] = LM_GGML_FP32_TO_FP16(arr[3]);
}

#define LM_GGML_F32Cx4             __m128
#define LM_GGML_F32Cx4_ZERO        _mm_setzero_ps()
#define LM_GGML_F32Cx4_SET1(x)     _mm_set1_ps(x)
#define LM_GGML_F32Cx4_LOAD(x)     __sse_f16x4_load(x)
#define LM_GGML_F32Cx4_STORE(x, y) __sse_f16x4_store(x, y)
#define LM_GGML_F32Cx4_FMA         LM_GGML_F32x4_FMA
#define LM_GGML_F32Cx4_ADD         _mm_add_ps
#define LM_GGML_F32Cx4_MUL         _mm_mul_ps
#define LM_GGML_F32Cx4_REDUCE      LM_GGML_F32x4_REDUCE

#define LM_GGML_F16_VEC                 LM_GGML_F32Cx4
#define LM_GGML_F16_VEC_ZERO            LM_GGML_F32Cx4_ZERO
#define LM_GGML_F16_VEC_SET1            LM_GGML_F32Cx4_SET1
#define LM_GGML_F16_VEC_LOAD(p, i)      LM_GGML_F32Cx4_LOAD(p)
#define LM_GGML_F16_VEC_STORE(p, r, i)  LM_GGML_F32Cx4_STORE(p, r[i])
#define LM_GGML_F16_VEC_FMA             LM_GGML_F32Cx4_FMA
#define LM_GGML_F16_VEC_ADD             LM_GGML_F32Cx4_ADD
#define LM_GGML_F16_VEC_MUL             LM_GGML_F32Cx4_MUL
#define LM_GGML_F16_VEC_REDUCE          LM_GGML_F32Cx4_REDUCE

#endif

// LM_GGML_F32_ARR / LM_GGML_F16_ARR
//   number of registers to use per step
#ifdef LM_GGML_SIMD
#define LM_GGML_F32_ARR (LM_GGML_F32_STEP/LM_GGML_F32_EPR)
#define LM_GGML_F16_ARR (LM_GGML_F16_STEP/LM_GGML_F16_EPR)
#endif

//
// fundamental operations
//

inline static void lm_ggml_vec_set_i8(const int n, int8_t * x, const int8_t v) { for (int i = 0; i < n; ++i) x[i] = v; }

inline static void lm_ggml_vec_set_i16(const int n, int16_t * x, const int16_t v) { for (int i = 0; i < n; ++i) x[i] = v; }

inline static void lm_ggml_vec_set_i32(const int n, int32_t * x, const int32_t v) { for (int i = 0; i < n; ++i) x[i] = v; }

inline static void lm_ggml_vec_set_f16(const int n, lm_ggml_fp16_t * x, const int32_t v) { for (int i = 0; i < n; ++i) x[i] = v; }

inline static void lm_ggml_vec_add_f32 (const int n, float * z, const float * x, const float * y) { for (int i = 0; i < n; ++i) z[i]  = x[i] + y[i]; }
inline static void lm_ggml_vec_add1_f32(const int n, float * z, const float * x, const float   v) { for (int i = 0; i < n; ++i) z[i]  = x[i] + v;    }
inline static void lm_ggml_vec_acc_f32 (const int n, float * y, const float * x)                  { for (int i = 0; i < n; ++i) y[i] += x[i];        }
inline static void lm_ggml_vec_acc1_f32(const int n, float * y, const float   v)                  { for (int i = 0; i < n; ++i) y[i] += v;           }
inline static void lm_ggml_vec_sub_f32 (const int n, float * z, const float * x, const float * y) { for (int i = 0; i < n; ++i) z[i]  = x[i] - y[i]; }
inline static void lm_ggml_vec_set_f32 (const int n, float * x, const float   v)                  { for (int i = 0; i < n; ++i) x[i]  = v;           }
inline static void lm_ggml_vec_cpy_f32 (const int n, float * y, const float * x)                  { for (int i = 0; i < n; ++i) y[i]  = x[i];        }
inline static void lm_ggml_vec_neg_f32 (const int n, float * y, const float * x)                  { for (int i = 0; i < n; ++i) y[i]  = -x[i];       }
inline static void lm_ggml_vec_mul_f32 (const int n, float * z, const float * x, const float * y) { for (int i = 0; i < n; ++i) z[i]  = x[i]*y[i];   }
inline static void lm_ggml_vec_div_f32 (const int n, float * z, const float * x, const float * y) { for (int i = 0; i < n; ++i) z[i]  = x[i]/y[i];   }

static void lm_ggml_vec_dot_f32(const int n, float * restrict s, const float * restrict x, const float * restrict y) {
#ifdef LM_GGML_SIMD
    float sumf = 0.0f;
    const int np = (n & ~(LM_GGML_F32_STEP - 1));

    LM_GGML_F32_VEC sum[LM_GGML_F32_ARR] = { LM_GGML_F32_VEC_ZERO };

    LM_GGML_F32_VEC ax[LM_GGML_F32_ARR];
    LM_GGML_F32_VEC ay[LM_GGML_F32_ARR];

    for (int i = 0; i < np; i += LM_GGML_F32_STEP) {
        for (int j = 0; j < LM_GGML_F32_ARR; j++) {
            ax[j] = LM_GGML_F32_VEC_LOAD(x + i + j*LM_GGML_F32_EPR);
            ay[j] = LM_GGML_F32_VEC_LOAD(y + i + j*LM_GGML_F32_EPR);

            sum[j] = LM_GGML_F32_VEC_FMA(sum[j], ax[j], ay[j]);
        }
    }

    // reduce sum0..sum3 to sum0
    LM_GGML_F32_VEC_REDUCE(sumf, sum);

    // leftovers
    for (int i = np; i < n; ++i) {
        sumf += x[i]*y[i];
    }
#else
    // scalar
    lm_ggml_float sumf = 0.0;
    for (int i = 0; i < n; ++i) {
        sumf += (lm_ggml_float)(x[i]*y[i]);
    }
#endif

    *s = sumf;
}

static void lm_ggml_vec_dot_f16(const int n, float * restrict s, lm_ggml_fp16_t * restrict x, lm_ggml_fp16_t * restrict y) {
    lm_ggml_float sumf = 0.0;

#if defined(LM_GGML_SIMD)
    const int np = (n & ~(LM_GGML_F16_STEP - 1));

    LM_GGML_F16_VEC sum[LM_GGML_F16_ARR] = { LM_GGML_F16_VEC_ZERO };

    LM_GGML_F16_VEC ax[LM_GGML_F16_ARR];
    LM_GGML_F16_VEC ay[LM_GGML_F16_ARR];

    for (int i = 0; i < np; i += LM_GGML_F16_STEP) {
        for (int j = 0; j < LM_GGML_F16_ARR; j++) {
            ax[j] = LM_GGML_F16_VEC_LOAD(x + i + j*LM_GGML_F16_EPR, j);
            ay[j] = LM_GGML_F16_VEC_LOAD(y + i + j*LM_GGML_F16_EPR, j);

            sum[j] = LM_GGML_F16_VEC_FMA(sum[j], ax[j], ay[j]);
        }
    }

    // reduce sum0..sum3 to sum0
    LM_GGML_F16_VEC_REDUCE(sumf, sum);

    // leftovers
    for (int i = np; i < n; ++i) {
        sumf += (lm_ggml_float)(LM_GGML_FP16_TO_FP32(x[i])*LM_GGML_FP16_TO_FP32(y[i]));
    }
#else
    for (int i = 0; i < n; ++i) {
        sumf += (lm_ggml_float)(LM_GGML_FP16_TO_FP32(x[i])*LM_GGML_FP16_TO_FP32(y[i]));
    }
#endif

    *s = sumf;
}

static void lm_ggml_vec_dot_q4_0_q8_0(const int n, float * restrict s, const void * restrict vx, const void * restrict vy) {
    const int qk = QK8_0;
    const int nb = n / qk;

    assert(n % qk == 0);

    const block_q4_0 * restrict x = vx;
    const block_q8_0 * restrict y = vy;

#if defined(__ARM_NEON)
    float32x4_t sumv0 = vdupq_n_f32(0.0f);
    float32x4_t sumv1 = vdupq_n_f32(0.0f);

    LM_GGML_ASSERT(nb % 2 == 0); // TODO: handle odd nb
    for (int i = 0; i < nb; i += 2) {
        const block_q4_0 * restrict x0 = &x[i + 0];
        const block_q4_0 * restrict x1 = &x[i + 1];
        const block_q8_0 * restrict y0 = &y[i + 0];
        const block_q8_0 * restrict y1 = &y[i + 1];

        const uint8x16_t m4b = vdupq_n_u8(0x0F);
        const int8x16_t  s8b = vdupq_n_s8(0x8);

        const uint8x16_t v0_0 = vld1q_u8(x0->qs);
        const uint8x16_t v0_1 = vld1q_u8(x1->qs);

        // 4-bit -> 8-bit
        const int8x16_t v0_0l = vreinterpretq_s8_u8(vandq_u8  (v0_0, m4b));
        const int8x16_t v0_0h = vreinterpretq_s8_u8(vshrq_n_u8(v0_0, 4));
        const int8x16_t v0_1l = vreinterpretq_s8_u8(vandq_u8  (v0_1, m4b));
        const int8x16_t v0_1h = vreinterpretq_s8_u8(vshrq_n_u8(v0_1, 4));

        // sub 8
        const int8x16_t v0_0ls = vsubq_s8(v0_0l, s8b);
        const int8x16_t v0_0hs = vsubq_s8(v0_0h, s8b);
        const int8x16_t v0_1ls = vsubq_s8(v0_1l, s8b);
        const int8x16_t v0_1hs = vsubq_s8(v0_1h, s8b);

        // load y
        const int8x16_t v1_0l = vld1q_s8(y0->qs);
        const int8x16_t v1_0h = vld1q_s8(y0->qs + 16);
        const int8x16_t v1_1l = vld1q_s8(y1->qs);
        const int8x16_t v1_1h = vld1q_s8(y1->qs + 16);

#if defined(__ARM_FEATURE_DOTPROD)
        // dot product into int32x4_t
        const int32x4_t p_0 = vdotq_s32(vdotq_s32(vdupq_n_s32(0), v0_0ls, v1_0l), v0_0hs, v1_0h);
        const int32x4_t p_1 = vdotq_s32(vdotq_s32(vdupq_n_s32(0), v0_1ls, v1_1l), v0_1hs, v1_1h);

        sumv0 = vmlaq_n_f32(sumv0, vcvtq_f32_s32(p_0), LM_GGML_FP16_TO_FP32(x0->d)*LM_GGML_FP16_TO_FP32(y0->d));
        sumv1 = vmlaq_n_f32(sumv1, vcvtq_f32_s32(p_1), LM_GGML_FP16_TO_FP32(x1->d)*LM_GGML_FP16_TO_FP32(y1->d));
#else
        const int16x8_t pl0l = vmull_s8(vget_low_s8 (v0_0ls), vget_low_s8 (v1_0l));
        const int16x8_t pl0h = vmull_s8(vget_high_s8(v0_0ls), vget_high_s8(v1_0l));
        const int16x8_t ph0l = vmull_s8(vget_low_s8 (v0_0hs), vget_low_s8 (v1_0h));
        const int16x8_t ph0h = vmull_s8(vget_high_s8(v0_0hs), vget_high_s8(v1_0h));

        const int16x8_t pl1l = vmull_s8(vget_low_s8 (v0_1ls), vget_low_s8 (v1_1l));
        const int16x8_t pl1h = vmull_s8(vget_high_s8(v0_1ls), vget_high_s8(v1_1l));
        const int16x8_t ph1l = vmull_s8(vget_low_s8 (v0_1hs), vget_low_s8 (v1_1h));
        const int16x8_t ph1h = vmull_s8(vget_high_s8(v0_1hs), vget_high_s8(v1_1h));

        const int32x4_t pl0 = vaddq_s32(vpaddlq_s16(pl0l), vpaddlq_s16(pl0h));
        const int32x4_t ph0 = vaddq_s32(vpaddlq_s16(ph0l), vpaddlq_s16(ph0h));
        const int32x4_t pl1 = vaddq_s32(vpaddlq_s16(pl1l), vpaddlq_s16(pl1h));
        const int32x4_t ph1 = vaddq_s32(vpaddlq_s16(ph1l), vpaddlq_s16(ph1h));

        sumv0 = vmlaq_n_f32(sumv0, vcvtq_f32_s32(vaddq_s32(pl0, ph0)), LM_GGML_FP16_TO_FP32(x0->d)*LM_GGML_FP16_TO_FP32(y0->d));
        sumv1 = vmlaq_n_f32(sumv1, vcvtq_f32_s32(vaddq_s32(pl1, ph1)), LM_GGML_FP16_TO_FP32(x1->d)*LM_GGML_FP16_TO_FP32(y1->d));
#endif
    }

    *s = vaddvq_f32(sumv0) + vaddvq_f32(sumv1);
#elif defined(__AVX2__)
    // Initialize accumulator with zeros
    __m256 acc = _mm256_setzero_ps();

    // Main loop
    for (int i = 0; i < nb; ++i) {
        /* Compute combined scale for the block */
        const __m256 d = _mm256_set1_ps( LM_GGML_FP16_TO_FP32(x[i].d) * LM_GGML_FP16_TO_FP32(y[i].d) );

        __m256i bx = bytes_from_nibbles_32(x[i].qs);

        // Now we have a vector with bytes in [ 0 .. 15 ] interval. Offset them into [ -8 .. +7 ] interval.
        const __m256i off = _mm256_set1_epi8( 8 );
        bx = _mm256_sub_epi8( bx, off );

        __m256i by = _mm256_loadu_si256((const __m256i *)y[i].qs);

        const __m256 q = mul_sum_i8_pairs_float(bx, by);

        /* Multiply q with scale and accumulate */
        acc = _mm256_fmadd_ps( d, q, acc );
    }

    *s = hsum_float_8(acc);
#elif defined(__AVX__)
    // Initialize accumulator with zeros
    __m256 acc = _mm256_setzero_ps();

    // Main loop
    for (int i = 0; i < nb; ++i) {
        // Compute combined scale for the block
        const __m256 d = _mm256_set1_ps( LM_GGML_FP16_TO_FP32(x[i].d) * LM_GGML_FP16_TO_FP32(y[i].d) );

        const __m128i lowMask = _mm_set1_epi8(0xF);
        const __m128i off = _mm_set1_epi8(8);

        const __m128i tmp = _mm_loadu_si128((const __m128i *)x[i].qs);

        __m128i bx = _mm_and_si128(lowMask, tmp);
        __m128i by = _mm_loadu_si128((const __m128i *)y[i].qs);
        bx = _mm_sub_epi8(bx, off);
        const __m128i i32_0 = mul_sum_i8_pairs(bx, by);

        bx = _mm_and_si128(lowMask, _mm_srli_epi64(tmp, 4));
        by = _mm_loadu_si128((const __m128i *)(y[i].qs + 16));
        bx = _mm_sub_epi8(bx, off);
        const __m128i i32_1 = mul_sum_i8_pairs(bx, by);

        // Convert int32_t to float
        __m256 p = _mm256_cvtepi32_ps(MM256_SET_M128I(i32_0, i32_1));

        // Apply the scale, and accumulate
        acc = _mm256_add_ps(_mm256_mul_ps( d, p ), acc);
    }

    *s = hsum_float_8(acc);
#elif defined(__SSSE3__)
    // set constants
    const __m128i lowMask = _mm_set1_epi8(0xF);
    const __m128i off = _mm_set1_epi8(8);

    // Initialize accumulator with zeros
    __m128 acc_0 = _mm_setzero_ps();
    __m128 acc_1 = _mm_setzero_ps();
    __m128 acc_2 = _mm_setzero_ps();
    __m128 acc_3 = _mm_setzero_ps();

    // First round without accumulation
    {
        _mm_prefetch(&x[0] + sizeof(block_q4_0), _MM_HINT_T0);
        _mm_prefetch(&y[0] + sizeof(block_q8_0), _MM_HINT_T0);

        // Compute combined scale for the block 0 and 1
        const __m128 d_0_1 = _mm_set1_ps( LM_GGML_FP16_TO_FP32(x[0].d) * LM_GGML_FP16_TO_FP32(y[0].d) );

        const __m128i tmp_0_1 = _mm_loadu_si128((const __m128i *)x[0].qs);

        __m128i bx_0 = _mm_and_si128(lowMask, tmp_0_1);
        __m128i by_0 = _mm_loadu_si128((const __m128i *)y[0].qs);
        bx_0 = _mm_sub_epi8(bx_0, off);
        const __m128i i32_0 = mul_sum_i8_pairs(bx_0, by_0);

        __m128i bx_1 = _mm_and_si128(lowMask, _mm_srli_epi64(tmp_0_1, 4));
        __m128i by_1 = _mm_loadu_si128((const __m128i *)(y[0].qs + 16));
        bx_1 = _mm_sub_epi8(bx_1, off);
        const __m128i i32_1 = mul_sum_i8_pairs(bx_1, by_1);

        _mm_prefetch(&x[1] + sizeof(block_q4_0), _MM_HINT_T0);
        _mm_prefetch(&y[1] + sizeof(block_q8_0), _MM_HINT_T0);

        // Compute combined scale for the block 2 and 3
        const __m128 d_2_3 = _mm_set1_ps( LM_GGML_FP16_TO_FP32(x[1].d) * LM_GGML_FP16_TO_FP32(y[1].d) );

        const __m128i tmp_2_3 = _mm_loadu_si128((const __m128i *)x[1].qs);

        __m128i bx_2 = _mm_and_si128(lowMask, tmp_2_3);
        __m128i by_2 = _mm_loadu_si128((const __m128i *)y[1].qs);
        bx_2 = _mm_sub_epi8(bx_2, off);
        const __m128i i32_2 = mul_sum_i8_pairs(bx_2, by_2);

        __m128i bx_3 = _mm_and_si128(lowMask, _mm_srli_epi64(tmp_2_3, 4));
        __m128i by_3 = _mm_loadu_si128((const __m128i *)(y[1].qs + 16));
        bx_3 = _mm_sub_epi8(bx_3, off);
        const __m128i i32_3 = mul_sum_i8_pairs(bx_3, by_3);

        // Convert int32_t to float
        __m128 p0 = _mm_cvtepi32_ps(i32_0);
        __m128 p1 = _mm_cvtepi32_ps(i32_1);
        __m128 p2 = _mm_cvtepi32_ps(i32_2);
        __m128 p3 = _mm_cvtepi32_ps(i32_3);

        // Apply the scale
        acc_0 = _mm_mul_ps( d_0_1, p0 );
        acc_1 = _mm_mul_ps( d_0_1, p1 );
        acc_2 = _mm_mul_ps( d_2_3, p2 );
        acc_3 = _mm_mul_ps( d_2_3, p3 );
    }

    // Main loop
    LM_GGML_ASSERT(nb % 2 == 0); // TODO: handle odd nb
    for (int i = 2; i < nb; i+=2) {
        _mm_prefetch(&x[i] + sizeof(block_q4_0), _MM_HINT_T0);
        _mm_prefetch(&y[i] + sizeof(block_q8_0), _MM_HINT_T0);

        // Compute combined scale for the block 0 and 1
        const __m128 d_0_1 = _mm_set1_ps( LM_GGML_FP16_TO_FP32(x[i].d) * LM_GGML_FP16_TO_FP32(y[i].d) );

        const __m128i tmp_0_1 = _mm_loadu_si128((const __m128i *)x[i].qs);

        __m128i bx_0 = _mm_and_si128(lowMask, tmp_0_1);
        __m128i by_0 = _mm_loadu_si128((const __m128i *)y[i].qs);
        bx_0 = _mm_sub_epi8(bx_0, off);
        const __m128i i32_0 = mul_sum_i8_pairs(bx_0, by_0);

        __m128i bx_1 = _mm_and_si128(lowMask, _mm_srli_epi64(tmp_0_1, 4));
        __m128i by_1 = _mm_loadu_si128((const __m128i *)(y[i].qs + 16));
        bx_1 = _mm_sub_epi8(bx_1, off);
        const __m128i i32_1 = mul_sum_i8_pairs(bx_1, by_1);

        _mm_prefetch(&x[i] + 2 * sizeof(block_q4_0), _MM_HINT_T0);
        _mm_prefetch(&y[i] + 2 * sizeof(block_q8_0), _MM_HINT_T0);

        // Compute combined scale for the block 2 and 3
        const __m128 d_2_3 = _mm_set1_ps( LM_GGML_FP16_TO_FP32(x[i + 1].d) * LM_GGML_FP16_TO_FP32(y[i + 1].d) );

        const __m128i tmp_2_3 = _mm_loadu_si128((const __m128i *)x[i + 1].qs);

        __m128i bx_2 = _mm_and_si128(lowMask, tmp_2_3);
        __m128i by_2 = _mm_loadu_si128((const __m128i *)y[i + 1].qs);
        bx_2 = _mm_sub_epi8(bx_2, off);
        const __m128i i32_2 = mul_sum_i8_pairs(bx_2, by_2);

        __m128i bx_3 = _mm_and_si128(lowMask, _mm_srli_epi64(tmp_2_3, 4));
        __m128i by_3 = _mm_loadu_si128((const __m128i *)(y[i + 1].qs + 16));
        bx_3 = _mm_sub_epi8(bx_3, off);
        const __m128i i32_3 = mul_sum_i8_pairs(bx_3, by_3);

        // Convert int32_t to float
        __m128 p0 = _mm_cvtepi32_ps(i32_0);
        __m128 p1 = _mm_cvtepi32_ps(i32_1);
        __m128 p2 = _mm_cvtepi32_ps(i32_2);
        __m128 p3 = _mm_cvtepi32_ps(i32_3);

        // Apply the scale
        __m128 p0_d = _mm_mul_ps( d_0_1, p0 );
        __m128 p1_d = _mm_mul_ps( d_0_1, p1 );
        __m128 p2_d = _mm_mul_ps( d_2_3, p2 );
        __m128 p3_d = _mm_mul_ps( d_2_3, p3 );

        // Acummulate
        acc_0 = _mm_add_ps(p0_d, acc_0);
        acc_1 = _mm_add_ps(p1_d, acc_1);
        acc_2 = _mm_add_ps(p2_d, acc_2);
        acc_3 = _mm_add_ps(p3_d, acc_3);
    }

    *s = hsum_float_4x4(acc_0, acc_1, acc_2, acc_3);
#elif defined(__riscv_v_intrinsic)
    float sumf = 0.0;

    size_t vl = __riscv_vsetvl_e8m1(qk/2);

    for (int i = 0; i < nb; i++) {
        // load elements
        vuint8mf2_t tx = __riscv_vle8_v_u8mf2(x[i].qs, vl);

        vint8mf2_t y0 = __riscv_vle8_v_i8mf2(y[i].qs, vl);
        vint8mf2_t y1 = __riscv_vle8_v_i8mf2(y[i].qs+16, vl);

        // mask and store lower part of x, and then upper part
        vuint8mf2_t x_a = __riscv_vand_vx_u8mf2(tx, 0x0F, vl);
        vuint8mf2_t x_l = __riscv_vsrl_vx_u8mf2(tx, 0x04, vl);

        vint8mf2_t x_ai = __riscv_vreinterpret_v_u8mf2_i8mf2(x_a);
        vint8mf2_t x_li = __riscv_vreinterpret_v_u8mf2_i8mf2(x_l);

        // subtract offset
        vint8mf2_t v0 = __riscv_vsub_vx_i8mf2(x_ai, 8, vl);
        vint8mf2_t v1 = __riscv_vsub_vx_i8mf2(x_li, 8, vl);

        vint16m1_t vec_mul1 = __riscv_vwmul_vv_i16m1(v0, y0, vl);
        vint16m1_t vec_mul2 = __riscv_vwmul_vv_i16m1(v1, y1, vl);

        vint32m1_t vec_zero = __riscv_vmv_v_x_i32m1(0, vl);

        vint32m1_t vs1 = __riscv_vwredsum_vs_i16m1_i32m1(vec_mul1, vec_zero, vl);
        vint32m1_t vs2 = __riscv_vwredsum_vs_i16m1_i32m1(vec_mul2, vs1, vl);

        int sumi = __riscv_vmv_x_s_i32m1_i32(vs2);

        sumf += sumi*LM_GGML_FP16_TO_FP32(x[i].d)*LM_GGML_FP16_TO_FP32(y[i].d);
    }

    *s = sumf;
#else
    // scalar
    float sumf = 0.0;

    for (int i = 0; i < nb; i++) {
        int sumi = 0;

        for (int j = 0; j < qk/2; ++j) {
            const int v0 = (x[i].qs[j] & 0x0F) - 8;
            const int v1 = (x[i].qs[j] >>   4) - 8;

            sumi += (v0 * y[i].qs[j]) + (v1 * y[i].qs[j + qk/2]);
        }

        sumf += sumi*LM_GGML_FP16_TO_FP32(x[i].d)*LM_GGML_FP16_TO_FP32(y[i].d);
    }

    *s = sumf;
#endif
}

static void lm_ggml_vec_dot_q4_1_q8_1(const int n, float * restrict s, const void * restrict vx, const void * restrict vy) {
    const int qk = QK8_1;
    const int nb = n / qk;

    assert(n % qk == 0);

    const block_q4_1 * restrict x = vx;
    const block_q8_1 * restrict y = vy;

    // TODO: add WASM SIMD
#if defined(__ARM_NEON)
    float32x4_t sumv0 = vdupq_n_f32(0.0f);
    float32x4_t sumv1 = vdupq_n_f32(0.0f);

    float summs = 0;

    LM_GGML_ASSERT(nb % 2 == 0); // TODO: handle odd nb
    for (int i = 0; i < nb; i += 2) {
        const block_q4_1 * restrict x0 = &x[i + 0];
        const block_q4_1 * restrict x1 = &x[i + 1];
        const block_q8_1 * restrict y0 = &y[i + 0];
        const block_q8_1 * restrict y1 = &y[i + 1];

        summs += LM_GGML_FP16_TO_FP32(x0->m) * y0->s + LM_GGML_FP16_TO_FP32(x1->m) * y1->s;

        const uint8x16_t m4b = vdupq_n_u8(0x0F);

        const uint8x16_t v0_0 = vld1q_u8(x0->qs);
        const uint8x16_t v0_1 = vld1q_u8(x1->qs);

        // 4-bit -> 8-bit
        const int8x16_t v0_0l = vreinterpretq_s8_u8(vandq_u8  (v0_0, m4b));
        const int8x16_t v0_0h = vreinterpretq_s8_u8(vshrq_n_u8(v0_0, 4));
        const int8x16_t v0_1l = vreinterpretq_s8_u8(vandq_u8  (v0_1, m4b));
        const int8x16_t v0_1h = vreinterpretq_s8_u8(vshrq_n_u8(v0_1, 4));

        // load y
        const int8x16_t v1_0l = vld1q_s8(y0->qs);
        const int8x16_t v1_0h = vld1q_s8(y0->qs + 16);
        const int8x16_t v1_1l = vld1q_s8(y1->qs);
        const int8x16_t v1_1h = vld1q_s8(y1->qs + 16);

#if defined(__ARM_FEATURE_DOTPROD)
        // dot product into int32x4_t
        const int32x4_t p_0 = vdotq_s32(vdotq_s32(vdupq_n_s32(0), v0_0l, v1_0l), v0_0h, v1_0h);
        const int32x4_t p_1 = vdotq_s32(vdotq_s32(vdupq_n_s32(0), v0_1l, v1_1l), v0_1h, v1_1h);

        sumv0 = vmlaq_n_f32(sumv0, vcvtq_f32_s32(p_0), LM_GGML_FP16_TO_FP32(x0->d)*y0->d);
        sumv1 = vmlaq_n_f32(sumv1, vcvtq_f32_s32(p_1), LM_GGML_FP16_TO_FP32(x1->d)*y1->d);
#else
        const int16x8_t pl0l = vmull_s8(vget_low_s8 (v0_0l), vget_low_s8 (v1_0l));
        const int16x8_t pl0h = vmull_s8(vget_high_s8(v0_0l), vget_high_s8(v1_0l));
        const int16x8_t ph0l = vmull_s8(vget_low_s8 (v0_0h), vget_low_s8 (v1_0h));
        const int16x8_t ph0h = vmull_s8(vget_high_s8(v0_0h), vget_high_s8(v1_0h));

        const int16x8_t pl1l = vmull_s8(vget_low_s8 (v0_1l), vget_low_s8 (v1_1l));
        const int16x8_t pl1h = vmull_s8(vget_high_s8(v0_1l), vget_high_s8(v1_1l));
        const int16x8_t ph1l = vmull_s8(vget_low_s8 (v0_1h), vget_low_s8 (v1_1h));
        const int16x8_t ph1h = vmull_s8(vget_high_s8(v0_1h), vget_high_s8(v1_1h));

        const int32x4_t pl0 = vaddq_s32(vpaddlq_s16(pl0l), vpaddlq_s16(pl0h));
        const int32x4_t ph0 = vaddq_s32(vpaddlq_s16(ph0l), vpaddlq_s16(ph0h));
        const int32x4_t pl1 = vaddq_s32(vpaddlq_s16(pl1l), vpaddlq_s16(pl1h));
        const int32x4_t ph1 = vaddq_s32(vpaddlq_s16(ph1l), vpaddlq_s16(ph1h));

        sumv0 = vmlaq_n_f32(sumv0, vcvtq_f32_s32(vaddq_s32(pl0, ph0)), LM_GGML_FP16_TO_FP32(x0->d)*y0->d);
        sumv1 = vmlaq_n_f32(sumv1, vcvtq_f32_s32(vaddq_s32(pl1, ph1)), LM_GGML_FP16_TO_FP32(x1->d)*y1->d);
#endif
    }

    *s = vaddvq_f32(sumv0) + vaddvq_f32(sumv1) + summs;
#elif defined(__AVX2__) || defined(__AVX__)
    // Initialize accumulator with zeros
    __m256 acc = _mm256_setzero_ps();

    float summs = 0;

    // Main loop
    for (int i = 0; i < nb; ++i) {
        const float d0 = LM_GGML_FP16_TO_FP32(x[i].d);
        const float d1 = y[i].d;

        summs += LM_GGML_FP16_TO_FP32(x[i].m) * y[i].s;

        const __m256 d0v = _mm256_set1_ps( d0 );
        const __m256 d1v = _mm256_set1_ps( d1 );

        // Compute combined scales
        const __m256 d0d1 = _mm256_mul_ps( d0v, d1v );

        // Load 16 bytes, and unpack 4 bit fields into bytes, making 32 bytes
        const __m256i bx = bytes_from_nibbles_32(x[i].qs);
        const __m256i by = _mm256_loadu_si256( (const __m256i *)y[i].qs );

        const __m256 xy = mul_sum_us8_pairs_float(bx, by);

        // Accumulate d0*d1*x*y
#if defined(__AVX2__)
        acc = _mm256_fmadd_ps( d0d1, xy, acc );
#else
        acc = _mm256_add_ps( _mm256_mul_ps( d0d1, xy ), acc );
#endif
    }

    *s = hsum_float_8(acc) + summs;
#elif defined(__riscv_v_intrinsic)
    float sumf = 0.0;

    size_t vl = __riscv_vsetvl_e8m1(qk/2);

    for (int i = 0; i < nb; i++) {
        // load elements
        vuint8mf2_t tx = __riscv_vle8_v_u8mf2(x[i].qs, vl);

        vint8mf2_t y0 = __riscv_vle8_v_i8mf2(y[i].qs, vl);
        vint8mf2_t y1 = __riscv_vle8_v_i8mf2(y[i].qs+16, vl);

        // mask and store lower part of x, and then upper part
        vuint8mf2_t x_a = __riscv_vand_vx_u8mf2(tx, 0x0F, vl);
        vuint8mf2_t x_l = __riscv_vsrl_vx_u8mf2(tx, 0x04, vl);

        vint8mf2_t v0 = __riscv_vreinterpret_v_u8mf2_i8mf2(x_a);
        vint8mf2_t v1 = __riscv_vreinterpret_v_u8mf2_i8mf2(x_l);

        vint16m1_t vec_mul1 = __riscv_vwmul_vv_i16m1(v0, y0, vl);
        vint16m1_t vec_mul2 = __riscv_vwmul_vv_i16m1(v1, y1, vl);

        vint32m1_t vec_zero = __riscv_vmv_v_x_i32m1(0, vl);

        vint32m1_t vs1 = __riscv_vwredsum_vs_i16m1_i32m1(vec_mul1, vec_zero, vl);
        vint32m1_t vs2 = __riscv_vwredsum_vs_i16m1_i32m1(vec_mul2, vs1, vl);

        int sumi = __riscv_vmv_x_s_i32m1_i32(vs2);

        sumf += (LM_GGML_FP16_TO_FP32(x[i].d)*y[i].d)*sumi + LM_GGML_FP16_TO_FP32(x[i].m)*y[i].s;
    }

    *s = sumf;
#else
    // scalar
    float sumf = 0.0;

    for (int i = 0; i < nb; i++) {
        int sumi = 0;

        for (int j = 0; j < qk/2; ++j) {
            const int v0 = (x[i].qs[j] & 0x0F);
            const int v1 = (x[i].qs[j] >>   4);

            sumi += (v0 * y[i].qs[j]) + (v1 * y[i].qs[j + qk/2]);
        }

        sumf += (LM_GGML_FP16_TO_FP32(x[i].d)*y[i].d)*sumi + LM_GGML_FP16_TO_FP32(x[i].m)*y[i].s;
    }

    *s = sumf;
#endif
}

static void lm_ggml_vec_dot_q5_0_q8_0(const int n, float * restrict s, const void * restrict vx, const void * restrict vy) {
    const int qk = QK8_0;
    const int nb = n / qk;

    assert(n % qk == 0);
    assert(qk == QK5_0);

    const block_q5_0 * restrict x = vx;
    const block_q8_0 * restrict y = vy;

#if defined(__ARM_NEON)
    float32x4_t sumv0 = vdupq_n_f32(0.0f);
    float32x4_t sumv1 = vdupq_n_f32(0.0f);

    uint32_t qh0;
    uint32_t qh1;

    uint64_t tmp0[4];
    uint64_t tmp1[4];

    LM_GGML_ASSERT(nb % 2 == 0); // TODO: handle odd nb
    for (int i = 0; i < nb; i += 2) {
        const block_q5_0 * restrict x0 = &x[i];
        const block_q5_0 * restrict x1 = &x[i + 1];
        const block_q8_0 * restrict y0 = &y[i];
        const block_q8_0 * restrict y1 = &y[i + 1];

        const uint8x16_t m4b = vdupq_n_u8(0x0F);

        // extract the 5th bit via lookup table ((!b) << 4)
        memcpy(&qh0, x0->qh, sizeof(qh0));
        memcpy(&qh1, x1->qh, sizeof(qh1));

        tmp0[0] = table_b2b_1[(qh0 >>  0) & 0xFF];
        tmp0[1] = table_b2b_1[(qh0 >>  8) & 0xFF];
        tmp0[2] = table_b2b_1[(qh0 >> 16) & 0xFF];
        tmp0[3] = table_b2b_1[(qh0 >> 24)       ];

        tmp1[0] = table_b2b_1[(qh1 >>  0) & 0xFF];
        tmp1[1] = table_b2b_1[(qh1 >>  8) & 0xFF];
        tmp1[2] = table_b2b_1[(qh1 >> 16) & 0xFF];
        tmp1[3] = table_b2b_1[(qh1 >> 24)       ];

        const int8x16_t qhl0 = vld1q_s8((const int8_t *)(tmp0 + 0));
        const int8x16_t qhh0 = vld1q_s8((const int8_t *)(tmp0 + 2));
        const int8x16_t qhl1 = vld1q_s8((const int8_t *)(tmp1 + 0));
        const int8x16_t qhh1 = vld1q_s8((const int8_t *)(tmp1 + 2));

        const uint8x16_t v0_0 = vld1q_u8(x0->qs);
        const uint8x16_t v0_1 = vld1q_u8(x1->qs);

        // 4-bit -> 8-bit
        int8x16_t v0_0l = vreinterpretq_s8_u8(vandq_u8  (v0_0, m4b));
        int8x16_t v0_0h = vreinterpretq_s8_u8(vshrq_n_u8(v0_0, 4));
        int8x16_t v0_1l = vreinterpretq_s8_u8(vandq_u8  (v0_1, m4b));
        int8x16_t v0_1h = vreinterpretq_s8_u8(vshrq_n_u8(v0_1, 4));

        // add high bit and sub 16 (equivalent to sub 0x10 when bit is zero)
        const int8x16_t v0_0lf = vsubq_s8(v0_0l, qhl0);
        const int8x16_t v0_0hf = vsubq_s8(v0_0h, qhh0);
        const int8x16_t v0_1lf = vsubq_s8(v0_1l, qhl1);
        const int8x16_t v0_1hf = vsubq_s8(v0_1h, qhh1);

        // load y
        const int8x16_t v1_0l = vld1q_s8(y0->qs);
        const int8x16_t v1_0h = vld1q_s8(y0->qs + 16);
        const int8x16_t v1_1l = vld1q_s8(y1->qs);
        const int8x16_t v1_1h = vld1q_s8(y1->qs + 16);

#if defined(__ARM_FEATURE_DOTPROD)
        sumv0 = vmlaq_n_f32(sumv0, vcvtq_f32_s32(vaddq_s32(
                        vdotq_s32(vdupq_n_s32(0), v0_0lf, v1_0l),
                        vdotq_s32(vdupq_n_s32(0), v0_0hf, v1_0h))), LM_GGML_FP16_TO_FP32(x0->d)*LM_GGML_FP16_TO_FP32(y0->d));
        sumv1 = vmlaq_n_f32(sumv1, vcvtq_f32_s32(vaddq_s32(
                        vdotq_s32(vdupq_n_s32(0), v0_1lf, v1_1l),
                        vdotq_s32(vdupq_n_s32(0), v0_1hf, v1_1h))), LM_GGML_FP16_TO_FP32(x1->d)*LM_GGML_FP16_TO_FP32(y1->d));
#else
        const int16x8_t pl0l = vmull_s8(vget_low_s8 (v0_0lf), vget_low_s8 (v1_0l));
        const int16x8_t pl0h = vmull_s8(vget_high_s8(v0_0lf), vget_high_s8(v1_0l));
        const int16x8_t ph0l = vmull_s8(vget_low_s8 (v0_0hf), vget_low_s8 (v1_0h));
        const int16x8_t ph0h = vmull_s8(vget_high_s8(v0_0hf), vget_high_s8(v1_0h));

        const int16x8_t pl1l = vmull_s8(vget_low_s8 (v0_1lf), vget_low_s8 (v1_1l));
        const int16x8_t pl1h = vmull_s8(vget_high_s8(v0_1lf), vget_high_s8(v1_1l));
        const int16x8_t ph1l = vmull_s8(vget_low_s8 (v0_1hf), vget_low_s8 (v1_1h));
        const int16x8_t ph1h = vmull_s8(vget_high_s8(v0_1hf), vget_high_s8(v1_1h));

        const int32x4_t pl0 = vaddq_s32(vpaddlq_s16(pl0l), vpaddlq_s16(pl0h));
        const int32x4_t ph0 = vaddq_s32(vpaddlq_s16(ph0l), vpaddlq_s16(ph0h));
        const int32x4_t pl1 = vaddq_s32(vpaddlq_s16(pl1l), vpaddlq_s16(pl1h));
        const int32x4_t ph1 = vaddq_s32(vpaddlq_s16(ph1l), vpaddlq_s16(ph1h));

        sumv0 = vmlaq_n_f32(sumv0, vcvtq_f32_s32(vaddq_s32(pl0, ph0)), LM_GGML_FP16_TO_FP32(x0->d)*LM_GGML_FP16_TO_FP32(y0->d));
        sumv1 = vmlaq_n_f32(sumv1, vcvtq_f32_s32(vaddq_s32(pl1, ph1)), LM_GGML_FP16_TO_FP32(x1->d)*LM_GGML_FP16_TO_FP32(y1->d));
#endif
    }

    *s = vaddvq_f32(sumv0) + vaddvq_f32(sumv1);
#elif defined(__wasm_simd128__)
    v128_t sumv = wasm_f32x4_splat(0.0f);

    uint32_t qh;
    uint64_t tmp[4];

    // TODO: check if unrolling this is better
    for (int i = 0; i < nb; ++i) {
        const block_q5_0 * restrict x0 = &x[i];
        const block_q8_0 * restrict y0 = &y[i];

        const v128_t m4b  = wasm_i8x16_splat(0x0F);

        // extract the 5th bit
        memcpy(&qh, x0->qh, sizeof(qh));

        tmp[0] = table_b2b_1[(qh >>  0) & 0xFF];
        tmp[1] = table_b2b_1[(qh >>  8) & 0xFF];
        tmp[2] = table_b2b_1[(qh >> 16) & 0xFF];
        tmp[3] = table_b2b_1[(qh >> 24)       ];

        const v128_t qhl = wasm_v128_load(tmp + 0);
        const v128_t qhh = wasm_v128_load(tmp + 2);

        const v128_t v0 = wasm_v128_load(x0->qs);

        // 4-bit -> 8-bit
        const v128_t v0l = wasm_v128_and (v0, m4b);
        const v128_t v0h = wasm_u8x16_shr(v0, 4);

        // add high bit and sub 16 (equivalent to sub 0x10 when bit is zero)
        const v128_t v0lf = wasm_i8x16_sub(v0l, qhl);
        const v128_t v0hf = wasm_i8x16_sub(v0h, qhh);

        // load y
        const v128_t v1l = wasm_v128_load(y0->qs);
        const v128_t v1h = wasm_v128_load(y0->qs + 16);

        // int8x16 -> int16x8
        const v128_t v0lfl = wasm_i16x8_extend_low_i8x16 (v0lf);
        const v128_t v0lfh = wasm_i16x8_extend_high_i8x16(v0lf);
        const v128_t v0hfl = wasm_i16x8_extend_low_i8x16 (v0hf);
        const v128_t v0hfh = wasm_i16x8_extend_high_i8x16(v0hf);

        const v128_t v1ll = wasm_i16x8_extend_low_i8x16 (v1l);
        const v128_t v1lh = wasm_i16x8_extend_high_i8x16(v1l);
        const v128_t v1hl = wasm_i16x8_extend_low_i8x16 (v1h);
        const v128_t v1hh = wasm_i16x8_extend_high_i8x16(v1h);

        // dot product
        sumv = wasm_f32x4_add(sumv, wasm_f32x4_mul(wasm_f32x4_convert_i32x4(
                        wasm_i32x4_add(
                            wasm_i32x4_add(wasm_i32x4_dot_i16x8(v0lfl, v1ll),
                                           wasm_i32x4_dot_i16x8(v0lfh, v1lh)),
                            wasm_i32x4_add(wasm_i32x4_dot_i16x8(v0hfl, v1hl),
                                           wasm_i32x4_dot_i16x8(v0hfh, v1hh)))),
                    wasm_f32x4_splat(LM_GGML_FP16_TO_FP32(x0->d) * LM_GGML_FP16_TO_FP32(y0->d))));
    }

    *s = wasm_f32x4_extract_lane(sumv, 0) + wasm_f32x4_extract_lane(sumv, 1) +
         wasm_f32x4_extract_lane(sumv, 2) + wasm_f32x4_extract_lane(sumv, 3);
#elif defined(__AVX2__)
    // Initialize accumulator with zeros
    __m256 acc = _mm256_setzero_ps();

    // Main loop
    for (int i = 0; i < nb; i++) {
        /* Compute combined scale for the block */
        const __m256 d = _mm256_set1_ps(LM_GGML_FP16_TO_FP32(x[i].d) * LM_GGML_FP16_TO_FP32(y[i].d));

        __m256i bx = bytes_from_nibbles_32(x[i].qs);
        __m256i bxhi = bytes_from_bits_32(x[i].qh);
        bxhi = _mm256_andnot_si256(bxhi, _mm256_set1_epi8((char)0xF0));
        bx = _mm256_or_si256(bx, bxhi);

        __m256i by = _mm256_loadu_si256((const __m256i *)y[i].qs);

        const __m256 q = mul_sum_i8_pairs_float(bx, by);

        /* Multiply q with scale and accumulate */
        acc = _mm256_fmadd_ps(d, q, acc);
    }

    *s = hsum_float_8(acc);
#elif defined(__AVX__)
    // Initialize accumulator with zeros
    __m256 acc = _mm256_setzero_ps();
    __m128i mask = _mm_set1_epi8((char)0xF0);

    // Main loop
    for (int i = 0; i < nb; i++) {
        /* Compute combined scale for the block */
        const __m256 d = _mm256_set1_ps(LM_GGML_FP16_TO_FP32(x[i].d) * LM_GGML_FP16_TO_FP32(y[i].d));

        __m256i bx = bytes_from_nibbles_32(x[i].qs);
        const __m256i bxhi = bytes_from_bits_32(x[i].qh);
        __m128i bxhil = _mm256_castsi256_si128(bxhi);
        __m128i bxhih = _mm256_extractf128_si256(bxhi, 1);
        bxhil = _mm_andnot_si128(bxhil, mask);
        bxhih = _mm_andnot_si128(bxhih, mask);
        __m128i bxl = _mm256_castsi256_si128(bx);
        __m128i bxh = _mm256_extractf128_si256(bx, 1);
        bxl = _mm_or_si128(bxl, bxhil);
        bxh = _mm_or_si128(bxh, bxhih);
        bx = MM256_SET_M128I(bxh, bxl);

        const __m256i by = _mm256_loadu_si256((const __m256i *)y[i].qs);

        const __m256 q = mul_sum_i8_pairs_float(bx, by);

        /* Multiply q with scale and accumulate */
        acc = _mm256_add_ps(_mm256_mul_ps(d, q), acc);
    }

    *s = hsum_float_8(acc);
#elif defined(__riscv_v_intrinsic)
    float sumf = 0.0;

    uint32_t qh;

    size_t vl = __riscv_vsetvl_e8m1(qk/2);

    // These tempory registers are for masking and shift operations
    vuint32m2_t vt_1 = __riscv_vid_v_u32m2(vl);
    vuint32m2_t vt_2 = __riscv_vsll_vv_u32m2(__riscv_vmv_v_x_u32m2(1, vl), vt_1, vl);

    vuint32m2_t vt_3 = __riscv_vsll_vx_u32m2(vt_2, 16, vl);
    vuint32m2_t vt_4 = __riscv_vadd_vx_u32m2(vt_1, 12, vl);

    for (int i = 0; i < nb; i++) {
        memcpy(&qh, x[i].qh, sizeof(uint32_t));

        // ((qh & (1u << (j + 0 ))) >> (j + 0 )) << 4;
        vuint32m2_t xha_0 = __riscv_vand_vx_u32m2(vt_2, qh, vl);
        vuint32m2_t xhr_0 = __riscv_vsrl_vv_u32m2(xha_0, vt_1, vl);
        vuint32m2_t xhl_0 = __riscv_vsll_vx_u32m2(xhr_0, 4, vl);

        // ((qh & (1u << (j + 16))) >> (j + 12));
        vuint32m2_t xha_1 = __riscv_vand_vx_u32m2(vt_3, qh, vl);
        vuint32m2_t xhl_1 = __riscv_vsrl_vv_u32m2(xha_1, vt_4, vl);

        // narrowing
        vuint16m1_t xhc_0 = __riscv_vncvt_x_x_w_u16m1(xhl_0, vl);
        vuint8mf2_t xh_0 = __riscv_vncvt_x_x_w_u8mf2(xhc_0, vl);

        vuint16m1_t xhc_1 = __riscv_vncvt_x_x_w_u16m1(xhl_1, vl);
        vuint8mf2_t xh_1 = __riscv_vncvt_x_x_w_u8mf2(xhc_1, vl);

        // load
        vuint8mf2_t tx = __riscv_vle8_v_u8mf2(x[i].qs, vl);

        vint8mf2_t y0 = __riscv_vle8_v_i8mf2(y[i].qs, vl);
        vint8mf2_t y1 = __riscv_vle8_v_i8mf2(y[i].qs+16, vl);

        vuint8mf2_t x_at = __riscv_vand_vx_u8mf2(tx, 0x0F, vl);
        vuint8mf2_t x_lt = __riscv_vsrl_vx_u8mf2(tx, 0x04, vl);

        vuint8mf2_t x_a = __riscv_vor_vv_u8mf2(x_at, xh_0, vl);
        vuint8mf2_t x_l = __riscv_vor_vv_u8mf2(x_lt, xh_1, vl);

        vint8mf2_t x_ai = __riscv_vreinterpret_v_u8mf2_i8mf2(x_a);
        vint8mf2_t x_li = __riscv_vreinterpret_v_u8mf2_i8mf2(x_l);

        vint8mf2_t v0 = __riscv_vsub_vx_i8mf2(x_ai, 16, vl);
        vint8mf2_t v1 = __riscv_vsub_vx_i8mf2(x_li, 16, vl);

        vint16m1_t vec_mul1 = __riscv_vwmul_vv_i16m1(v0, y0, vl);
        vint16m1_t vec_mul2 = __riscv_vwmul_vv_i16m1(v1, y1, vl);

        vint32m1_t vec_zero = __riscv_vmv_v_x_i32m1(0, vl);

        vint32m1_t vs1 = __riscv_vwredsum_vs_i16m1_i32m1(vec_mul1, vec_zero, vl);
        vint32m1_t vs2 = __riscv_vwredsum_vs_i16m1_i32m1(vec_mul2, vs1, vl);

        int sumi = __riscv_vmv_x_s_i32m1_i32(vs2);

        sumf += (LM_GGML_FP16_TO_FP32(x[i].d)*LM_GGML_FP16_TO_FP32(y[i].d)) * sumi;
    }

    *s = sumf;
#else
    // scalar
    float sumf = 0.0;

    for (int i = 0; i < nb; i++) {
        uint32_t qh;
        memcpy(&qh, x[i].qh, sizeof(qh));

        int sumi = 0;

        for (int j = 0; j < qk/2; ++j) {
            const uint8_t xh_0 = ((qh & (1u << (j + 0 ))) >> (j + 0 )) << 4;
            const uint8_t xh_1 = ((qh & (1u << (j + 16))) >> (j + 12));

            const int32_t x0 = ((x[i].qs[j] & 0x0F) | xh_0) - 16;
            const int32_t x1 = ((x[i].qs[j] >>   4) | xh_1) - 16;

            sumi += (x0 * y[i].qs[j]) + (x1 * y[i].qs[j + qk/2]);
        }

        sumf += (LM_GGML_FP16_TO_FP32(x[i].d)*LM_GGML_FP16_TO_FP32(y[i].d)) * sumi;
    }

    *s = sumf;
#endif
}

static void lm_ggml_vec_dot_q5_1_q8_1(const int n, float * restrict s, const void * restrict vx, const void * restrict vy) {
    const int qk = QK8_1;
    const int nb = n / qk;

    assert(n % qk == 0);
    assert(qk == QK5_1);

    const block_q5_1 * restrict x = vx;
    const block_q8_1 * restrict y = vy;

#if defined(__ARM_NEON)
    float32x4_t sumv0 = vdupq_n_f32(0.0f);
    float32x4_t sumv1 = vdupq_n_f32(0.0f);

    float summs0 = 0.0f;
    float summs1 = 0.0f;

    uint32_t qh0;
    uint32_t qh1;

    uint64_t tmp0[4];
    uint64_t tmp1[4];

    LM_GGML_ASSERT(nb % 2 == 0); // TODO: handle odd nb
    for (int i = 0; i < nb; i += 2) {
        const block_q5_1 * restrict x0 = &x[i];
        const block_q5_1 * restrict x1 = &x[i + 1];
        const block_q8_1 * restrict y0 = &y[i];
        const block_q8_1 * restrict y1 = &y[i + 1];

        const uint8x16_t m4b = vdupq_n_u8(0x0F);

        summs0 += LM_GGML_FP16_TO_FP32(x0->m) * y0->s;
        summs1 += LM_GGML_FP16_TO_FP32(x1->m) * y1->s;

        // extract the 5th bit via lookup table ((b) << 4)
        memcpy(&qh0, x0->qh, sizeof(qh0));
        memcpy(&qh1, x1->qh, sizeof(qh1));

        tmp0[0] = table_b2b_0[(qh0 >>  0) & 0xFF];
        tmp0[1] = table_b2b_0[(qh0 >>  8) & 0xFF];
        tmp0[2] = table_b2b_0[(qh0 >> 16) & 0xFF];
        tmp0[3] = table_b2b_0[(qh0 >> 24)       ];

        tmp1[0] = table_b2b_0[(qh1 >>  0) & 0xFF];
        tmp1[1] = table_b2b_0[(qh1 >>  8) & 0xFF];
        tmp1[2] = table_b2b_0[(qh1 >> 16) & 0xFF];
        tmp1[3] = table_b2b_0[(qh1 >> 24)       ];

        const int8x16_t qhl0 = vld1q_s8((const int8_t *)(tmp0 + 0));
        const int8x16_t qhh0 = vld1q_s8((const int8_t *)(tmp0 + 2));
        const int8x16_t qhl1 = vld1q_s8((const int8_t *)(tmp1 + 0));
        const int8x16_t qhh1 = vld1q_s8((const int8_t *)(tmp1 + 2));

        const uint8x16_t v0_0 = vld1q_u8(x0->qs);
        const uint8x16_t v0_1 = vld1q_u8(x1->qs);

        // 4-bit -> 8-bit
        const int8x16_t v0_0l = vreinterpretq_s8_u8(vandq_u8  (v0_0, m4b));
        const int8x16_t v0_0h = vreinterpretq_s8_u8(vshrq_n_u8(v0_0, 4));
        const int8x16_t v0_1l = vreinterpretq_s8_u8(vandq_u8  (v0_1, m4b));
        const int8x16_t v0_1h = vreinterpretq_s8_u8(vshrq_n_u8(v0_1, 4));

        // add high bit
        const int8x16_t v0_0lf = vorrq_s8(v0_0l, qhl0);
        const int8x16_t v0_0hf = vorrq_s8(v0_0h, qhh0);
        const int8x16_t v0_1lf = vorrq_s8(v0_1l, qhl1);
        const int8x16_t v0_1hf = vorrq_s8(v0_1h, qhh1);

        // load y
        const int8x16_t v1_0l = vld1q_s8(y0->qs);
        const int8x16_t v1_0h = vld1q_s8(y0->qs + 16);
        const int8x16_t v1_1l = vld1q_s8(y1->qs);
        const int8x16_t v1_1h = vld1q_s8(y1->qs + 16);

#if defined(__ARM_FEATURE_DOTPROD)
        sumv0 = vmlaq_n_f32(sumv0, vcvtq_f32_s32(vaddq_s32(
                        vdotq_s32(vdupq_n_s32(0), v0_0lf, v1_0l),
                        vdotq_s32(vdupq_n_s32(0), v0_0hf, v1_0h))), LM_GGML_FP16_TO_FP32(x0->d)*y0->d);
        sumv1 = vmlaq_n_f32(sumv1, vcvtq_f32_s32(vaddq_s32(
                        vdotq_s32(vdupq_n_s32(0), v0_1lf, v1_1l),
                        vdotq_s32(vdupq_n_s32(0), v0_1hf, v1_1h))), LM_GGML_FP16_TO_FP32(x1->d)*y1->d);
#else
        const int16x8_t pl0l = vmull_s8(vget_low_s8 (v0_0lf), vget_low_s8 (v1_0l));
        const int16x8_t pl0h = vmull_s8(vget_high_s8(v0_0lf), vget_high_s8(v1_0l));
        const int16x8_t ph0l = vmull_s8(vget_low_s8 (v0_0hf), vget_low_s8 (v1_0h));
        const int16x8_t ph0h = vmull_s8(vget_high_s8(v0_0hf), vget_high_s8(v1_0h));

        const int16x8_t pl1l = vmull_s8(vget_low_s8 (v0_1lf), vget_low_s8 (v1_1l));
        const int16x8_t pl1h = vmull_s8(vget_high_s8(v0_1lf), vget_high_s8(v1_1l));
        const int16x8_t ph1l = vmull_s8(vget_low_s8 (v0_1hf), vget_low_s8 (v1_1h));
        const int16x8_t ph1h = vmull_s8(vget_high_s8(v0_1hf), vget_high_s8(v1_1h));

        const int32x4_t pl0 = vaddq_s32(vpaddlq_s16(pl0l), vpaddlq_s16(pl0h));
        const int32x4_t ph0 = vaddq_s32(vpaddlq_s16(ph0l), vpaddlq_s16(ph0h));
        const int32x4_t pl1 = vaddq_s32(vpaddlq_s16(pl1l), vpaddlq_s16(pl1h));
        const int32x4_t ph1 = vaddq_s32(vpaddlq_s16(ph1l), vpaddlq_s16(ph1h));

        sumv0 = vmlaq_n_f32(sumv0, vcvtq_f32_s32(vaddq_s32(pl0, ph0)), LM_GGML_FP16_TO_FP32(x0->d)*y0->d);
        sumv1 = vmlaq_n_f32(sumv1, vcvtq_f32_s32(vaddq_s32(pl1, ph1)), LM_GGML_FP16_TO_FP32(x1->d)*y1->d);
#endif
    }

    *s = vaddvq_f32(sumv0) + vaddvq_f32(sumv1) + summs0 + summs1;
#elif defined(__wasm_simd128__)
    v128_t sumv = wasm_f32x4_splat(0.0f);

    float summs = 0.0f;

    uint32_t qh;
    uint64_t tmp[4];

    // TODO: check if unrolling this is better
    for (int i = 0; i < nb; ++i) {
        const block_q5_1 * restrict x0 = &x[i];
        const block_q8_1 * restrict y0 = &y[i];

        summs += LM_GGML_FP16_TO_FP32(x0->m) * y0->s;

        const v128_t m4b = wasm_i8x16_splat(0x0F);

        // extract the 5th bit
        memcpy(&qh, x0->qh, sizeof(qh));

        tmp[0] = table_b2b_0[(qh >>  0) & 0xFF];
        tmp[1] = table_b2b_0[(qh >>  8) & 0xFF];
        tmp[2] = table_b2b_0[(qh >> 16) & 0xFF];
        tmp[3] = table_b2b_0[(qh >> 24)       ];

        const v128_t qhl = wasm_v128_load(tmp + 0);
        const v128_t qhh = wasm_v128_load(tmp + 2);

        const v128_t v0 = wasm_v128_load(x0->qs);

        // 4-bit -> 8-bit
        const v128_t v0l = wasm_v128_and (v0, m4b);
        const v128_t v0h = wasm_u8x16_shr(v0, 4);

        // add high bit
        const v128_t v0lf = wasm_v128_or(v0l, qhl);
        const v128_t v0hf = wasm_v128_or(v0h, qhh);

        // load y
        const v128_t v1l = wasm_v128_load(y0->qs);
        const v128_t v1h = wasm_v128_load(y0->qs + 16);

        // int8x16 -> int16x8
        const v128_t v0lfl = wasm_i16x8_extend_low_i8x16 (v0lf);
        const v128_t v0lfh = wasm_i16x8_extend_high_i8x16(v0lf);
        const v128_t v0hfl = wasm_i16x8_extend_low_i8x16 (v0hf);
        const v128_t v0hfh = wasm_i16x8_extend_high_i8x16(v0hf);

        const v128_t v1ll = wasm_i16x8_extend_low_i8x16 (v1l);
        const v128_t v1lh = wasm_i16x8_extend_high_i8x16(v1l);
        const v128_t v1hl = wasm_i16x8_extend_low_i8x16 (v1h);
        const v128_t v1hh = wasm_i16x8_extend_high_i8x16(v1h);

        // dot product
        sumv = wasm_f32x4_add(sumv,
                wasm_f32x4_mul(wasm_f32x4_convert_i32x4(wasm_i32x4_add(
                            wasm_i32x4_add(wasm_i32x4_dot_i16x8(v0lfl, v1ll),
                                           wasm_i32x4_dot_i16x8(v0lfh, v1lh)),
                            wasm_i32x4_add(wasm_i32x4_dot_i16x8(v0hfl, v1hl),
                                           wasm_i32x4_dot_i16x8(v0hfh, v1hh)))),
                    wasm_f32x4_splat(LM_GGML_FP16_TO_FP32(x0->d) * y0->d)));
    }

    *s = wasm_f32x4_extract_lane(sumv, 0) + wasm_f32x4_extract_lane(sumv, 1) +
         wasm_f32x4_extract_lane(sumv, 2) + wasm_f32x4_extract_lane(sumv, 3) + summs;
#elif defined(__AVX2__)
    // Initialize accumulator with zeros
    __m256 acc = _mm256_setzero_ps();

    float summs = 0.0f;

    // Main loop
    for (int i = 0; i < nb; i++) {
        const __m256 dx = _mm256_set1_ps(LM_GGML_FP16_TO_FP32(x[i].d));

        summs += LM_GGML_FP16_TO_FP32(x[i].m) * y[i].s;

        __m256i bx = bytes_from_nibbles_32(x[i].qs);
        __m256i bxhi = bytes_from_bits_32(x[i].qh);
        bxhi = _mm256_and_si256(bxhi, _mm256_set1_epi8(0x10));
        bx = _mm256_or_si256(bx, bxhi);

        const __m256 dy = _mm256_set1_ps(y[i].d);
        const __m256i by = _mm256_loadu_si256((const __m256i *)y[i].qs);

        const __m256 q = mul_sum_us8_pairs_float(bx, by);

        acc = _mm256_fmadd_ps(q, _mm256_mul_ps(dx, dy), acc);
    }

    *s = hsum_float_8(acc) + summs;
#elif defined(__AVX__)
    // Initialize accumulator with zeros
    __m256 acc = _mm256_setzero_ps();
    __m128i mask = _mm_set1_epi8(0x10);

    float summs = 0.0f;

    // Main loop
    for (int i = 0; i < nb; i++) {
        const __m256 dx = _mm256_set1_ps(LM_GGML_FP16_TO_FP32(x[i].d));

        summs += LM_GGML_FP16_TO_FP32(x[i].m) * y[i].s;

        __m256i bx = bytes_from_nibbles_32(x[i].qs);
        const __m256i bxhi = bytes_from_bits_32(x[i].qh);
        __m128i bxhil = _mm256_castsi256_si128(bxhi);
        __m128i bxhih = _mm256_extractf128_si256(bxhi, 1);
        bxhil = _mm_and_si128(bxhil, mask);
        bxhih = _mm_and_si128(bxhih, mask);
        __m128i bxl = _mm256_castsi256_si128(bx);
        __m128i bxh = _mm256_extractf128_si256(bx, 1);
        bxl = _mm_or_si128(bxl, bxhil);
        bxh = _mm_or_si128(bxh, bxhih);
        bx = MM256_SET_M128I(bxh, bxl);

        const __m256 dy = _mm256_set1_ps(y[i].d);
        const __m256i by = _mm256_loadu_si256((const __m256i *)y[i].qs);

        const __m256 q = mul_sum_us8_pairs_float(bx, by);

        acc = _mm256_add_ps(_mm256_mul_ps(q, _mm256_mul_ps(dx, dy)), acc);
    }

    *s = hsum_float_8(acc) + summs;
#elif defined(__riscv_v_intrinsic)
    float sumf = 0.0;

    uint32_t qh;

    size_t vl = __riscv_vsetvl_e8m1(qk/2);

    // temporary registers for shift operations
    vuint32m2_t vt_1 = __riscv_vid_v_u32m2(vl);
    vuint32m2_t vt_2 = __riscv_vadd_vx_u32m2(vt_1, 12, vl);

    for (int i = 0; i < nb; i++) {
        memcpy(&qh, x[i].qh, sizeof(uint32_t));

        // load qh
        vuint32m2_t vqh = __riscv_vmv_v_x_u32m2(qh, vl);

        // ((qh >> (j +  0)) << 4) & 0x10;
        vuint32m2_t xhr_0 = __riscv_vsrl_vv_u32m2(vqh, vt_1, vl);
        vuint32m2_t xhl_0 = __riscv_vsll_vx_u32m2(xhr_0, 4, vl);
        vuint32m2_t xha_0 = __riscv_vand_vx_u32m2(xhl_0, 0x10, vl);

        // ((qh >> (j + 12))     ) & 0x10;
        vuint32m2_t xhr_1 = __riscv_vsrl_vv_u32m2(vqh, vt_2, vl);
        vuint32m2_t xha_1 = __riscv_vand_vx_u32m2(xhr_1, 0x10, vl);

        // narrowing
        vuint16m1_t xhc_0 = __riscv_vncvt_x_x_w_u16m1(xha_0, vl);
        vuint8mf2_t xh_0 = __riscv_vncvt_x_x_w_u8mf2(xhc_0, vl);

        vuint16m1_t xhc_1 = __riscv_vncvt_x_x_w_u16m1(xha_1, vl);
        vuint8mf2_t xh_1 = __riscv_vncvt_x_x_w_u8mf2(xhc_1, vl);

        // load
        vuint8mf2_t tx = __riscv_vle8_v_u8mf2(x[i].qs, vl);

        vint8mf2_t y0 = __riscv_vle8_v_i8mf2(y[i].qs, vl);
        vint8mf2_t y1 = __riscv_vle8_v_i8mf2(y[i].qs+16, vl);

        vuint8mf2_t x_at = __riscv_vand_vx_u8mf2(tx, 0x0F, vl);
        vuint8mf2_t x_lt = __riscv_vsrl_vx_u8mf2(tx, 0x04, vl);

        vuint8mf2_t x_a = __riscv_vor_vv_u8mf2(x_at, xh_0, vl);
        vuint8mf2_t x_l = __riscv_vor_vv_u8mf2(x_lt, xh_1, vl);

        vint8mf2_t v0 = __riscv_vreinterpret_v_u8mf2_i8mf2(x_a);
        vint8mf2_t v1 = __riscv_vreinterpret_v_u8mf2_i8mf2(x_l);

        vint16m1_t vec_mul1 = __riscv_vwmul_vv_i16m1(v0, y0, vl);
        vint16m1_t vec_mul2 = __riscv_vwmul_vv_i16m1(v1, y1, vl);

        vint32m1_t vec_zero = __riscv_vmv_v_x_i32m1(0, vl);

        vint32m1_t vs1 = __riscv_vwredsum_vs_i16m1_i32m1(vec_mul1, vec_zero, vl);
        vint32m1_t vs2 = __riscv_vwredsum_vs_i16m1_i32m1(vec_mul2, vs1, vl);

        int sumi = __riscv_vmv_x_s_i32m1_i32(vs2);

        sumf += (LM_GGML_FP16_TO_FP32(x[i].d)*y[i].d)*sumi + LM_GGML_FP16_TO_FP32(x[i].m)*y[i].s;
    }

    *s = sumf;
#else
    // scalar
    float sumf = 0.0;

    for (int i = 0; i < nb; i++) {
        uint32_t qh;
        memcpy(&qh, x[i].qh, sizeof(qh));

        int sumi = 0;

        for (int j = 0; j < qk/2; ++j) {
            const uint8_t xh_0 = ((qh >> (j +  0)) << 4) & 0x10;
            const uint8_t xh_1 = ((qh >> (j + 12))     ) & 0x10;

            const int32_t x0 = (x[i].qs[j] & 0xF) | xh_0;
            const int32_t x1 = (x[i].qs[j] >>  4) | xh_1;

            sumi += (x0 * y[i].qs[j]) + (x1 * y[i].qs[j + qk/2]);
        }

        sumf += (LM_GGML_FP16_TO_FP32(x[i].d)*y[i].d)*sumi + LM_GGML_FP16_TO_FP32(x[i].m)*y[i].s;
    }

    *s = sumf;
#endif
}

static void lm_ggml_vec_dot_q8_0_q8_0(const int n, float * restrict s, const void * restrict vx, const void * restrict vy) {
    const int qk = QK8_0;
    const int nb = n / qk;

    assert(n % qk == 0);

    const block_q8_0 * restrict x = vx;
    const block_q8_0 * restrict y = vy;

#if defined(__ARM_NEON)
    float32x4_t sumv0 = vdupq_n_f32(0.0f);
    float32x4_t sumv1 = vdupq_n_f32(0.0f);

    LM_GGML_ASSERT(nb % 2 == 0); // TODO: handle odd nb
    for (int i = 0; i < nb; i += 2) {
        const block_q8_0 * restrict x0 = &x[i + 0];
        const block_q8_0 * restrict x1 = &x[i + 1];
        const block_q8_0 * restrict y0 = &y[i + 0];
        const block_q8_0 * restrict y1 = &y[i + 1];

        const int8x16_t x0_0 = vld1q_s8(x0->qs);
        const int8x16_t x0_1 = vld1q_s8(x0->qs + 16);
        const int8x16_t x1_0 = vld1q_s8(x1->qs);
        const int8x16_t x1_1 = vld1q_s8(x1->qs + 16);

        // load y
        const int8x16_t y0_0 = vld1q_s8(y0->qs);
        const int8x16_t y0_1 = vld1q_s8(y0->qs + 16);
        const int8x16_t y1_0 = vld1q_s8(y1->qs);
        const int8x16_t y1_1 = vld1q_s8(y1->qs + 16);

#if defined(__ARM_FEATURE_DOTPROD)
        sumv0 = vmlaq_n_f32(sumv0, vcvtq_f32_s32(vaddq_s32(
                        vdotq_s32(vdupq_n_s32(0), x0_0, y0_0),
                        vdotq_s32(vdupq_n_s32(0), x0_1, y0_1))), LM_GGML_FP16_TO_FP32(x0->d)*LM_GGML_FP16_TO_FP32(y0->d));

        sumv1 = vmlaq_n_f32(sumv1, vcvtq_f32_s32(vaddq_s32(
                        vdotq_s32(vdupq_n_s32(0), x1_0, y1_0),
                        vdotq_s32(vdupq_n_s32(0), x1_1, y1_1))), LM_GGML_FP16_TO_FP32(x1->d)*LM_GGML_FP16_TO_FP32(y1->d));

#else
        const int16x8_t p0_0 = vmull_s8(vget_low_s8 (x0_0), vget_low_s8 (y0_0));
        const int16x8_t p0_1 = vmull_s8(vget_high_s8(x0_0), vget_high_s8(y0_0));
        const int16x8_t p0_2 = vmull_s8(vget_low_s8 (x0_1), vget_low_s8 (y0_1));
        const int16x8_t p0_3 = vmull_s8(vget_high_s8(x0_1), vget_high_s8(y0_1));

        const int16x8_t p1_0 = vmull_s8(vget_low_s8 (x1_0), vget_low_s8 (y1_0));
        const int16x8_t p1_1 = vmull_s8(vget_high_s8(x1_0), vget_high_s8(y1_0));
        const int16x8_t p1_2 = vmull_s8(vget_low_s8 (x1_1), vget_low_s8 (y1_1));
        const int16x8_t p1_3 = vmull_s8(vget_high_s8(x1_1), vget_high_s8(y1_1));

        const int32x4_t p0 = vaddq_s32(vpaddlq_s16(p0_0), vpaddlq_s16(p0_1));
        const int32x4_t p1 = vaddq_s32(vpaddlq_s16(p0_2), vpaddlq_s16(p0_3));
        const int32x4_t p2 = vaddq_s32(vpaddlq_s16(p1_0), vpaddlq_s16(p1_1));
        const int32x4_t p3 = vaddq_s32(vpaddlq_s16(p1_2), vpaddlq_s16(p1_3));

        sumv0 = vmlaq_n_f32(sumv0, vcvtq_f32_s32(vaddq_s32(p0, p1)), LM_GGML_FP16_TO_FP32(x0->d)*LM_GGML_FP16_TO_FP32(y0->d));
        sumv1 = vmlaq_n_f32(sumv1, vcvtq_f32_s32(vaddq_s32(p2, p3)), LM_GGML_FP16_TO_FP32(x1->d)*LM_GGML_FP16_TO_FP32(y1->d));
#endif
    }

    *s = vaddvq_f32(sumv0) + vaddvq_f32(sumv1);
#elif defined(__AVX2__) || defined(__AVX__)
    // Initialize accumulator with zeros
    __m256 acc = _mm256_setzero_ps();

    // Main loop
    for (int i = 0; i < nb; ++i) {
        // Compute combined scale for the block
        const __m256 d = _mm256_set1_ps(LM_GGML_FP16_TO_FP32(x[i].d) * LM_GGML_FP16_TO_FP32(y[i].d));
        __m256i bx = _mm256_loadu_si256((const __m256i *)x[i].qs);
        __m256i by = _mm256_loadu_si256((const __m256i *)y[i].qs);

        const __m256 q = mul_sum_i8_pairs_float(bx, by);

        // Multiply q with scale and accumulate
#if defined(__AVX2__)
        acc = _mm256_fmadd_ps( d, q, acc );
#else
        acc = _mm256_add_ps( _mm256_mul_ps( d, q ), acc );
#endif
    }

    *s = hsum_float_8(acc);
#elif defined(__riscv_v_intrinsic)
    float sumf = 0.0;
    size_t vl = __riscv_vsetvl_e8m1(qk);

    for (int i = 0; i < nb; i++) {
        // load elements
        vint8m1_t bx = __riscv_vle8_v_i8m1(x[i].qs, vl);
        vint8m1_t by = __riscv_vle8_v_i8m1(y[i].qs, vl);

        vint16m2_t vw_mul = __riscv_vwmul_vv_i16m2(bx, by, vl);

        vint32m1_t v_zero = __riscv_vmv_v_x_i32m1(0, vl);
        vint32m1_t v_sum = __riscv_vwredsum_vs_i16m2_i32m1(vw_mul, v_zero, vl);

        int sumi = __riscv_vmv_x_s_i32m1_i32(v_sum);

        sumf += sumi*(LM_GGML_FP16_TO_FP32(x[i].d)*LM_GGML_FP16_TO_FP32(y[i].d));
    }

    *s = sumf;
#else
    // scalar
    float sumf = 0.0;

    for (int i = 0; i < nb; i++) {
        int sumi = 0;

        for (int j = 0; j < qk; j++) {
            sumi += x[i].qs[j]*y[i].qs[j];
        }

        sumf += sumi*(LM_GGML_FP16_TO_FP32(x[i].d)*LM_GGML_FP16_TO_FP32(y[i].d));
    }

    *s = sumf;
#endif
}

// compute LM_GGML_VEC_DOT_UNROLL dot products at once
// xs - x row stride in bytes
inline static void lm_ggml_vec_dot_f16_unroll(const int n, const int xs, float * restrict s, void * restrict xv, lm_ggml_fp16_t * restrict y) {
    lm_ggml_float sumf[LM_GGML_VEC_DOT_UNROLL] = { 0.0 };

    lm_ggml_fp16_t * restrict x[LM_GGML_VEC_DOT_UNROLL];

    for (int i = 0; i < LM_GGML_VEC_DOT_UNROLL; ++i) {
        x[i] = (lm_ggml_fp16_t *) ((char *) xv + i*xs);
    }

#if defined(LM_GGML_SIMD)
    const int np = (n & ~(LM_GGML_F16_STEP - 1));

    LM_GGML_F16_VEC sum[LM_GGML_VEC_DOT_UNROLL][LM_GGML_F16_ARR] = { { LM_GGML_F16_VEC_ZERO } };

    LM_GGML_F16_VEC ax[LM_GGML_F16_ARR];
    LM_GGML_F16_VEC ay[LM_GGML_F16_ARR];

    for (int i = 0; i < np; i += LM_GGML_F16_STEP) {
        for (int j = 0; j < LM_GGML_F16_ARR; j++) {
            ay[j] = LM_GGML_F16_VEC_LOAD(y + i + j*LM_GGML_F16_EPR, j);

            for (int k = 0; k < LM_GGML_VEC_DOT_UNROLL; ++k) {
                ax[j] = LM_GGML_F16_VEC_LOAD(x[k] + i + j*LM_GGML_F16_EPR, j);

                sum[k][j] = LM_GGML_F16_VEC_FMA(sum[k][j], ax[j], ay[j]);
            }
        }
    }

    // reduce sum0..sum3 to sum0
    for (int k = 0; k < LM_GGML_VEC_DOT_UNROLL; ++k) {
        LM_GGML_F16_VEC_REDUCE(sumf[k], sum[k]);
    }

    // leftovers
    for (int i = np; i < n; ++i) {
        for (int j = 0; j < LM_GGML_VEC_DOT_UNROLL; ++j) {
            sumf[j] += (lm_ggml_float)(LM_GGML_FP16_TO_FP32(x[j][i])*LM_GGML_FP16_TO_FP32(y[i]));
        }
    }
#else
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < LM_GGML_VEC_DOT_UNROLL; ++j) {
            sumf[j] += (lm_ggml_float)(LM_GGML_FP16_TO_FP32(x[j][i])*LM_GGML_FP16_TO_FP32(y[i]));
        }
    }
#endif

    for (int i = 0; i < LM_GGML_VEC_DOT_UNROLL; ++i) {
        s[i] = sumf[i];
    }
}

inline static void lm_ggml_vec_mad_f32(const int n, float * restrict y, const float * restrict x, const float v) {
#if defined(LM_GGML_SIMD)
    const int np = (n & ~(LM_GGML_F32_STEP - 1));

    LM_GGML_F32_VEC vx = LM_GGML_F32_VEC_SET1(v);

    LM_GGML_F32_VEC ax[LM_GGML_F32_ARR];
    LM_GGML_F32_VEC ay[LM_GGML_F32_ARR];

    for (int i = 0; i < np; i += LM_GGML_F32_STEP) {
        for (int j = 0; j < LM_GGML_F32_ARR; j++) {
            ax[j] = LM_GGML_F32_VEC_LOAD(x + i + j*LM_GGML_F32_EPR);
            ay[j] = LM_GGML_F32_VEC_LOAD(y + i + j*LM_GGML_F32_EPR);
            ay[j] = LM_GGML_F32_VEC_FMA(ay[j], ax[j], vx);

            LM_GGML_F32_VEC_STORE(y + i + j*LM_GGML_F32_EPR, ay[j]);
        }
    }

    // leftovers
    for (int i = np; i < n; ++i) {
        y[i] += x[i]*v;
    }
#else
    // scalar
    for (int i = 0; i < n; ++i) {
        y[i] += x[i]*v;
    }
#endif
}

// xs and vs are byte strides of x and v
inline static void lm_ggml_vec_mad_f32_unroll(const int n, const int xs, const int vs, float * restrict y, const float * restrict xv, const float * restrict vv) {

    const float * restrict x[LM_GGML_VEC_MAD_UNROLL];
    const float * restrict v[LM_GGML_VEC_MAD_UNROLL];

    for (int i = 0; i < LM_GGML_VEC_MAD_UNROLL; ++i) {
        x[i] = (const float *) ((const char *) xv + i*xs);
        v[i] = (const float *) ((const char *) vv + i*vs);
    }

#if defined(LM_GGML_SIMD)
    const int np = (n & ~(LM_GGML_F32_STEP - 1));

    LM_GGML_F32_VEC vx[LM_GGML_VEC_MAD_UNROLL];

    for (int k = 0; k < LM_GGML_VEC_MAD_UNROLL; ++k) {
        vx[k] = LM_GGML_F32_VEC_SET1(v[k][0]);
    }

    LM_GGML_F32_VEC ax[LM_GGML_VEC_MAD_UNROLL][LM_GGML_F32_ARR];
    LM_GGML_F32_VEC ay[LM_GGML_F32_ARR];

    for (int i = 0; i < np; i += LM_GGML_F32_STEP) {
        for (int j = 0; j < LM_GGML_F32_ARR; j++) {
            ay[j] = LM_GGML_F32_VEC_LOAD(y + i + j*LM_GGML_F32_EPR);

            for (int k = 0; k < LM_GGML_VEC_MAD_UNROLL; ++k) {
                ax[k][j] = LM_GGML_F32_VEC_LOAD(x[k] + i + j*LM_GGML_F32_EPR);
                ay[j] = LM_GGML_F32_VEC_FMA(ay[j], ax[k][j], vx[k]);
            }

            LM_GGML_F32_VEC_STORE(y + i + j*LM_GGML_F32_EPR, ay[j]);
        }
    }

    // leftovers
    for (int k = 0; k < LM_GGML_VEC_MAD_UNROLL; ++k) {
        for (int i = np; i < n; ++i) {
            y[i] += x[k][i]*v[k][0];
        }
    }
#else
    // scalar
    for (int k = 0; k < LM_GGML_VEC_MAD_UNROLL; ++k) {
        for (int i = 0; i < n; ++i) {
            y[i] += x[k][i]*v[k][0];
        }
    }
#endif
}

//inline static void lm_ggml_vec_scale_f32(const int n, float * y, const float   v) { for (int i = 0; i < n; ++i) y[i] *= v;          }
inline static void lm_ggml_vec_scale_f32(const int n, float * y, const float   v) {
#if defined(LM_GGML_USE_ACCELERATE)
    vDSP_vsmul(y, 1, &v, y, 1, n);
#elif defined(LM_GGML_SIMD)
    const int np = (n & ~(LM_GGML_F32_STEP - 1));

    LM_GGML_F32_VEC vx = LM_GGML_F32_VEC_SET1(v);

    LM_GGML_F32_VEC ay[LM_GGML_F32_ARR];

    for (int i = 0; i < np; i += LM_GGML_F32_STEP) {
        for (int j = 0; j < LM_GGML_F32_ARR; j++) {
            ay[j] = LM_GGML_F32_VEC_LOAD(y + i + j*LM_GGML_F32_EPR);
            ay[j] = LM_GGML_F32_VEC_MUL(ay[j], vx);

            LM_GGML_F32_VEC_STORE(y + i + j*LM_GGML_F32_EPR, ay[j]);
        }
    }

    // leftovers
    for (int i = np; i < n; ++i) {
        y[i] *= v;
    }
#else
    // scalar
    for (int i = 0; i < n; ++i) {
        y[i] *= v;
    }
#endif
}

inline static void lm_ggml_vec_norm_f32 (const int n, float * s, const float * x) { lm_ggml_vec_dot_f32(n, s, x, x); *s = sqrtf(*s);   }
inline static void lm_ggml_vec_sqr_f32  (const int n, float * y, const float * x) { for (int i = 0; i < n; ++i) y[i] = x[i]*x[i];   }
inline static void lm_ggml_vec_sqrt_f32 (const int n, float * y, const float * x) { for (int i = 0; i < n; ++i) y[i] = sqrtf(x[i]); }
inline static void lm_ggml_vec_log_f32  (const int n, float * y, const float * x) { for (int i = 0; i < n; ++i) y[i] = logf(x[i]);   }
inline static void lm_ggml_vec_abs_f32  (const int n, float * y, const float * x) { for (int i = 0; i < n; ++i) y[i] = fabsf(x[i]); }
inline static void lm_ggml_vec_sgn_f32  (const int n, float * y, const float * x) { for (int i = 0; i < n; ++i) y[i] = (x[i] > 0.f) ? 1.f : ((x[i] < 0.f) ? -1.f : 0.f); }
inline static void lm_ggml_vec_step_f32 (const int n, float * y, const float * x) { for (int i = 0; i < n; ++i) y[i] = (x[i] > 0.f) ? 1.f : 0.f; }
inline static void lm_ggml_vec_tanh_f32 (const int n, float * y, const float * x) { for (int i = 0; i < n; ++i) y[i] = tanhf(x[i]);  }
inline static void lm_ggml_vec_elu_f32  (const int n, float * y, const float * x) { for (int i = 0; i < n; ++i) y[i] = (x[i] > 0.f) ? x[i] : expf(x[i])-1; }
inline static void lm_ggml_vec_relu_f32 (const int n, float * y, const float * x) { for (int i = 0; i < n; ++i) y[i] = (x[i] > 0.f) ? x[i] : 0.f; }

static const float GELU_COEF_A     = 0.044715f;
static const float GELU_QUICK_COEF = -1.702f;
static const float SQRT_2_OVER_PI  = 0.79788456080286535587989211986876f;

inline static float lm_ggml_gelu_f32(float x) {
    return 0.5f*x*(1.0f + tanhf(SQRT_2_OVER_PI*x*(1.0f + GELU_COEF_A*x*x)));
}

inline static void lm_ggml_vec_gelu_f16(const int n, lm_ggml_fp16_t * y, const lm_ggml_fp16_t * x) {
    const uint16_t * i16 = (const uint16_t *) x;
    for (int i = 0; i < n; ++i) {
        y[i] = table_gelu_f16[i16[i]];
    }
}

#ifdef LM_GGML_GELU_FP16
inline static void lm_ggml_vec_gelu_f32(const int n, float * y, const float * x) {
    uint16_t t;
    for (int i = 0; i < n; ++i) {
        lm_ggml_fp16_t fp16 = LM_GGML_FP32_TO_FP16(x[i]);
        memcpy(&t, &fp16, sizeof(uint16_t));
        y[i] = LM_GGML_FP16_TO_FP32(table_gelu_f16[t]);
    }
}
#else
inline static void lm_ggml_vec_gelu_f32(const int n, float * y, const float * x) {
    for (int i = 0; i < n; ++i) {
        y[i] = lm_ggml_gelu_f32(x[i]);
    }
}
#endif

inline static float lm_ggml_gelu_quick_f32(float x) {
    return x*(1.0f/(1.0f+expf(GELU_QUICK_COEF*x)));
}

//inline static void lm_ggml_vec_gelu_quick_f16(const int n, lm_ggml_fp16_t * y, const lm_ggml_fp16_t * x) {
//    const uint16_t * i16 = (const uint16_t *) x;
//    for (int i = 0; i < n; ++i) {
//        y[i] = table_gelu_quick_f16[i16[i]];
//    }
//}

#ifdef LM_GGML_GELU_QUICK_FP16
inline static void lm_ggml_vec_gelu_quick_f32(const int n, float * y, const float * x) {
    uint16_t t;
    for (int i = 0; i < n; ++i) {
        lm_ggml_fp16_t fp16 = LM_GGML_FP32_TO_FP16(x[i]);
        memcpy(&t, &fp16, sizeof(uint16_t));
        y[i] = LM_GGML_FP16_TO_FP32(table_gelu_quick_f16[t]);
    }
}
#else
inline static void lm_ggml_vec_gelu_quick_f32(const int n, float * y, const float * x) {
    for (int i = 0; i < n; ++i) {
        y[i] = lm_ggml_gelu_quick_f32(x[i]);
    }
}
#endif

// Sigmoid Linear Unit (SiLU) function
inline static float lm_ggml_silu_f32(float x) {
    return x/(1.0f + expf(-x));
}

//inline static void lm_ggml_vec_silu_f16(const int n, lm_ggml_fp16_t * y, const lm_ggml_fp16_t * x) {
//    const uint16_t * i16 = (const uint16_t *) x;
//    for (int i = 0; i < n; ++i) {
//        y[i] = table_silu_f16[i16[i]];
//    }
//}

#ifdef LM_GGML_SILU_FP16
inline static void lm_ggml_vec_silu_f32(const int n, float * y, const float * x) {
    uint16_t t;
    for (int i = 0; i < n; ++i) {
        lm_ggml_fp16_t fp16 = LM_GGML_FP32_TO_FP16(x[i]);
        memcpy(&t, &fp16, sizeof(uint16_t));
        y[i] = LM_GGML_FP16_TO_FP32(table_silu_f16[t]);
    }
}
#else
inline static void lm_ggml_vec_silu_f32(const int n, float * y, const float * x) {
    for (int i = 0; i < n; ++i) {
        y[i] = lm_ggml_silu_f32(x[i]);
    }
}
#endif

inline static float lm_ggml_silu_backward_f32(float x, float dy) {
    const float s = 1.0f/(1.0f + expf(-x));
    return dy*s*(1.0f + x*(1.0f - s));
}

#ifdef LM_GGML_SILU_FP16
inline static void lm_ggml_vec_silu_backward_f32(const int n, float * dx, const float * x, const float * dy) {
    for (int i = 0; i < n; ++i) {
        // we did not use x[i] to compute forward silu but its f16 equivalent
        // take derivative at f16 of x[i]:
        lm_ggml_fp16_t fp16 = LM_GGML_FP32_TO_FP16(x[i]);
        float usedx = LM_GGML_FP16_TO_FP32(fp16);
        dx[i] = lm_ggml_silu_backward_f32(usedx, dy[i]);
    }
}
#else
inline static void lm_ggml_vec_silu_backward_f32(const int n, float * dx, const float * x, const float * dy) {
    for (int i = 0; i < n; ++i) {
        dx[i] = lm_ggml_silu_backward_f32(x[i], dy[i]);
    }
}
#endif

inline static void lm_ggml_vec_sum_f32(const int n, float * s, const float * x) {
#ifndef LM_GGML_USE_ACCELERATE
    lm_ggml_float sum = 0.0;
    for (int i = 0; i < n; ++i) {
        sum += (lm_ggml_float)x[i];
    }
    *s = sum;
#else
    vDSP_sve(x, 1, s, n);
#endif
}

inline static void lm_ggml_vec_sum_f32_ggf(const int n, lm_ggml_float * s, const float * x) {
    lm_ggml_float sum = 0.0;
    for (int i = 0; i < n; ++i) {
        sum += (lm_ggml_float)x[i];
    }
    *s = sum;
}

inline static void lm_ggml_vec_sum_f16_ggf(const int n, float * s, const lm_ggml_fp16_t * x) {
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) {
        sum += LM_GGML_FP16_TO_FP32(x[i]);
    }
    *s = sum;
}

inline static void lm_ggml_vec_max_f32(const int n, float * s, const float * x) {
#ifndef LM_GGML_USE_ACCELERATE
    float max = -INFINITY;
    for (int i = 0; i < n; ++i) {
        max = MAX(max, x[i]);
    }
    *s = max;
#else
    vDSP_maxv(x, 1, s, n);
#endif
}

inline static void lm_ggml_vec_norm_inv_f32(const int n, float * s, const float * x) {
    lm_ggml_vec_norm_f32(n, s, x);
    *s = 1.f/(*s);
}

inline static void lm_ggml_vec_argmax_f32(const int n, int * s, const float * x) {
    float max = -INFINITY;
    int idx = 0;
    for (int i = 0; i < n; ++i) {
        max = MAX(max, x[i]);
        if (max == x[i]) { idx = i; }
    }
    *s = idx;
}

//
// data types
//

static const char * LM_GGML_OP_NAME[LM_GGML_OP_COUNT] = {
    "NONE",

    "DUP",
    "ADD",
    "ADD1",
    "ACC",
    "SUB",
    "MUL",
    "DIV",
    "SQR",
    "SQRT",
    "LOG",
    "SUM",
    "SUM_ROWS",
    "MEAN",
    "ARGMAX",
    "REPEAT",
    "REPEAT_BACK",
    "CONCAT",
    "SILU_BACK",
    "NORM",
    "RMS_NORM",
    "RMS_NORM_BACK",
    "GROUP_NORM",

    "MUL_MAT",
    "OUT_PROD",

    "SCALE",
    "SET",
    "CPY",
    "CONT",
    "RESHAPE",
    "VIEW",
    "PERMUTE",
    "TRANSPOSE",
    "GET_ROWS",
    "GET_ROWS_BACK",
    "DIAG",
    "DIAG_MASK_INF",
    "DIAG_MASK_ZERO",
    "SOFT_MAX",
    "SOFT_MAX_BACK",
    "ROPE",
    "ROPE_BACK",
    "ALIBI",
    "CLAMP",
    "CONV_1D",
    "CONV_1D_STAGE_0",
    "CONV_1D_STAGE_1",
    "CONV_TRANSPOSE_1D",
    "CONV_2D",
    "CONV_2D_STAGE_0",
    "CONV_2D_STAGE_1",
    "CONV_TRANSPOSE_2D",
    "POOL_1D",
    "POOL_2D",
    "UPSCALE",

    "FLASH_ATTN",
    "FLASH_FF",
    "FLASH_ATTN_BACK",
    "WIN_PART",
    "WIN_UNPART",
    "GET_REL_POS",
    "ADD_REL_POS",

    "UNARY",

    "MAP_UNARY",
    "MAP_BINARY",

    "MAP_CUSTOM1_F32",
    "MAP_CUSTOM2_F32",
    "MAP_CUSTOM3_F32",

    "MAP_CUSTOM1",
    "MAP_CUSTOM2",
    "MAP_CUSTOM3",

    "CROSS_ENTROPY_LOSS",
    "CROSS_ENTROPY_LOSS_BACK",
};

static_assert(LM_GGML_OP_COUNT == 73, "LM_GGML_OP_COUNT != 73");

static const char * LM_GGML_OP_SYMBOL[LM_GGML_OP_COUNT] = {
    "none",

    "x",
    "x+y",
    "x+y",
    "view(x,nb,offset)+=y->x",
    "x-y",
    "x*y",
    "x/y",
    "x^2",
    "√x",
    "log(x)",
    "Σx",
    "Σx_k",
    "Σx/n",
    "argmax(x)",
    "repeat(x)",
    "repeat_back(x)",
    "concat(x, y)",
    "silu_back(x)",
    "norm(x)",
    "rms_norm(x)",
    "rms_norm_back(x)",
    "group_norm(x)",

    "X*Y",
    "X*Y",

    "x*v",
    "y-\\>view(x)",
    "x-\\>y",
    "cont(x)",
    "reshape(x)",
    "view(x)",
    "permute(x)",
    "transpose(x)",
    "get_rows(x)",
    "get_rows_back(x)",
    "diag(x)",
    "diag_mask_inf(x)",
    "diag_mask_zero(x)",
    "soft_max(x)",
    "soft_max_back(x)",
    "rope(x)",
    "rope_back(x)",
    "alibi(x)",
    "clamp(x)",
    "conv_1d(x)",
    "conv_1d_stage_0(x)",
    "conv_1d_stage_1(x)",
    "conv_transpose_1d(x)",
    "conv_2d(x)",
    "conv_2d_stage_0(x)",
    "conv_2d_stage_1(x)",
    "conv_transpose_2d(x)",
    "pool_1d(x)",
    "pool_2d(x)",
    "upscale(x)",

    "flash_attn(x)",
    "flash_ff(x)",
    "flash_attn_back(x)",
    "win_part(x)",
    "win_unpart(x)",
    "get_rel_pos(x)",
    "add_rel_pos(x)",

    "unary(x)",

    "f(x)",
    "f(x,y)",

    "custom_f32(x)",
    "custom_f32(x,y)",
    "custom_f32(x,y,z)",

    "custom(x)",
    "custom(x,y)",
    "custom(x,y,z)",

    "cross_entropy_loss(x,y)",
    "cross_entropy_loss_back(x,y)",
};

static_assert(LM_GGML_OP_COUNT == 73, "LM_GGML_OP_COUNT != 73");

static_assert(LM_GGML_OP_POOL_COUNT == 2, "LM_GGML_OP_POOL_COUNT != 2");

static_assert(sizeof(struct lm_ggml_object)%LM_GGML_MEM_ALIGN == 0, "lm_ggml_object size must be a multiple of LM_GGML_MEM_ALIGN");
static_assert(sizeof(struct lm_ggml_tensor)%LM_GGML_MEM_ALIGN == 0, "lm_ggml_tensor size must be a multiple of LM_GGML_MEM_ALIGN");

// WARN:
// Mis-confguration can lead to problem that's hard to reason about:
// * At best  it crash or talks nosense.
// * At worst it talks slightly difference but hard to perceive.
//
// An op has to enable INIT or FINALIZE when any of it's branch needs that pass.
// Take care about compile options (e.g., LM_GGML_USE_xxx).
static bool LM_GGML_OP_HAS_INIT    [LM_GGML_OP_COUNT] = { 0 };
static bool LM_GGML_OP_HAS_FINALIZE[LM_GGML_OP_COUNT] = { 0 };

static void lm_ggml_setup_op_has_task_pass(void) {
    {   // INIT
        bool * p = LM_GGML_OP_HAS_INIT;

        p[LM_GGML_OP_ACC                    ] = true;
        p[LM_GGML_OP_MUL_MAT                ] = true;
        p[LM_GGML_OP_OUT_PROD               ] = true;
        p[LM_GGML_OP_SET                    ] = true;
        p[LM_GGML_OP_GET_ROWS_BACK          ] = true;
        p[LM_GGML_OP_DIAG_MASK_INF          ] = true;
        p[LM_GGML_OP_DIAG_MASK_ZERO         ] = true;
        p[LM_GGML_OP_CONV_1D                ] = true;
        p[LM_GGML_OP_CONV_1D_STAGE_0        ] = true;
        p[LM_GGML_OP_CONV_1D_STAGE_1        ] = true;
        p[LM_GGML_OP_CONV_TRANSPOSE_1D      ] = true;
        p[LM_GGML_OP_CONV_2D                ] = true;
        p[LM_GGML_OP_CONV_2D_STAGE_0        ] = true;
        p[LM_GGML_OP_CONV_2D_STAGE_1        ] = true;
        p[LM_GGML_OP_CONV_TRANSPOSE_2D      ] = true;
        p[LM_GGML_OP_FLASH_ATTN_BACK        ] = true;
        p[LM_GGML_OP_CROSS_ENTROPY_LOSS     ] = true;
        p[LM_GGML_OP_ADD_REL_POS            ] = true;
    }

    {   // FINALIZE
        bool * p = LM_GGML_OP_HAS_FINALIZE;

        p[LM_GGML_OP_CROSS_ENTROPY_LOSS     ] = true;
    }
}

//
// ggml context
//

struct lm_ggml_context {
    size_t mem_size;
    void * mem_buffer;
    bool   mem_buffer_owned;
    bool   no_alloc;
    bool   no_alloc_save; // this is used to save the no_alloc state when using scratch buffers

    int    n_objects;

    struct lm_ggml_object * objects_begin;
    struct lm_ggml_object * objects_end;

    struct lm_ggml_scratch scratch;
    struct lm_ggml_scratch scratch_save;
};

struct lm_ggml_context_container {
    bool used;

    struct lm_ggml_context context;
};

//
// NUMA support
//

#define LM_GGML_NUMA_MAX_NODES 8
#define LM_GGML_NUMA_MAX_CPUS 512

struct lm_ggml_numa_node {
    uint32_t cpus[LM_GGML_NUMA_MAX_CPUS]; // hardware threads on this node
    uint32_t n_cpus;
};

struct lm_ggml_numa_nodes {
    struct lm_ggml_numa_node nodes[LM_GGML_NUMA_MAX_NODES];
    uint32_t n_nodes;
    uint32_t total_cpus; // hardware threads on system
};

//
// ggml state
//

struct lm_ggml_state {
    struct lm_ggml_context_container contexts[LM_GGML_MAX_CONTEXTS];
    struct lm_ggml_numa_nodes numa;
};

// global state
static struct lm_ggml_state g_state;
static atomic_int g_state_barrier = 0;

// barrier via spin lock
inline static void lm_ggml_critical_section_start(void) {
    int processing = atomic_fetch_add(&g_state_barrier, 1);

    while (processing > 0) {
        // wait for other threads to finish
        atomic_fetch_sub(&g_state_barrier, 1);
        sched_yield(); // TODO: reconsider this
        processing = atomic_fetch_add(&g_state_barrier, 1);
    }
}

// TODO: make this somehow automatically executed
//       some sort of "sentry" mechanism
inline static void lm_ggml_critical_section_end(void) {
    atomic_fetch_sub(&g_state_barrier, 1);
}

void lm_ggml_numa_init(void) {
    if (g_state.numa.n_nodes > 0) {
        fprintf(stderr, "lm_ggml_numa_init: NUMA already initialized\n");

        return;
    }

#ifdef __linux__
    struct stat st;
    char path[256];
    int rv;

    // enumerate nodes
    while (g_state.numa.n_nodes < LM_GGML_NUMA_MAX_NODES) {
        rv = snprintf(path, sizeof(path), "/sys/devices/system/node/node%u", g_state.numa.n_nodes);
        LM_GGML_ASSERT(rv > 0 && (unsigned)rv < sizeof(path));
        if (stat(path, &st) != 0) { break; }
        ++g_state.numa.n_nodes;
    }

    // enumerate CPUs
    while (g_state.numa.total_cpus < LM_GGML_NUMA_MAX_CPUS) {
        rv = snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u", g_state.numa.total_cpus);
        LM_GGML_ASSERT(rv > 0 && (unsigned)rv < sizeof(path));
        if (stat(path, &st) != 0) { break; }
        ++g_state.numa.total_cpus;
    }

    LM_GGML_PRINT_DEBUG("found %u numa nodes, %u CPUs\n", g_state.numa.n_nodes, g_state.numa.total_cpus);

    if (g_state.numa.n_nodes < 1 || g_state.numa.total_cpus < 1) {
        g_state.numa.n_nodes = 0;
        return;
    }

    for (uint32_t n = 0; n < g_state.numa.n_nodes; ++n) {
        struct lm_ggml_numa_node * node = &g_state.numa.nodes[n];
        LM_GGML_PRINT_DEBUG("CPUs on node %u:", n);
        node->n_cpus = 0;
        for (uint32_t c = 0; c < g_state.numa.total_cpus; ++c) {
            rv = snprintf(path, sizeof(path), "/sys/devices/system/node/node%u/cpu%u", n, c);
            LM_GGML_ASSERT(rv > 0 && (unsigned)rv < sizeof(path));
            if (stat(path, &st) == 0) {
                node->cpus[node->n_cpus++] = c;
                LM_GGML_PRINT_DEBUG(" %u", c);
            }
        }
        LM_GGML_PRINT_DEBUG("\n");
    }

    if (lm_ggml_is_numa()) {
        FILE *fptr = fopen("/proc/sys/kernel/numa_balancing", "r");
        if (fptr != NULL) {
            char buf[42];
            if (fgets(buf, sizeof(buf), fptr) && strncmp(buf, "0\n", sizeof(buf)) != 0) {
                LM_GGML_PRINT("WARNING: /proc/sys/kernel/numa_balancing is enabled, this has been observed to impair performance\n");
            }
            fclose(fptr);
        }
    }
#else
    // TODO
#endif
}

bool lm_ggml_is_numa(void) {
    return g_state.numa.n_nodes > 1;
}

////////////////////////////////////////////////////////////////////////////////

void lm_ggml_print_object(const struct lm_ggml_object * obj) {
    LM_GGML_PRINT(" - lm_ggml_object: type = %d, offset = %zu, size = %zu, next = %p\n",
            obj->type, obj->offs, obj->size, (const void *) obj->next);
}

void lm_ggml_print_objects(const struct lm_ggml_context * ctx) {
    struct lm_ggml_object * obj = ctx->objects_begin;

    LM_GGML_PRINT("%s: objects in context %p:\n", __func__, (const void *) ctx);

    while (obj != NULL) {
        lm_ggml_print_object(obj);
        obj = obj->next;
    }

    LM_GGML_PRINT("%s: --- end ---\n", __func__);
}

int64_t lm_ggml_nelements(const struct lm_ggml_tensor * tensor) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return tensor->ne[0]*tensor->ne[1]*tensor->ne[2]*tensor->ne[3];
}

int64_t lm_ggml_nrows(const struct lm_ggml_tensor * tensor) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return tensor->ne[1]*tensor->ne[2]*tensor->ne[3];
}

size_t lm_ggml_nbytes(const struct lm_ggml_tensor * tensor) {
    size_t nbytes;
    size_t blck_size = lm_ggml_blck_size(tensor->type);
    if (blck_size == 1) {
        nbytes = lm_ggml_type_size(tensor->type);
        for (int i = 0; i < LM_GGML_MAX_DIMS; ++i) {
            nbytes += (tensor->ne[i] - 1)*tensor->nb[i];
        }
    }
    else {
        nbytes = tensor->ne[0]*tensor->nb[0]/blck_size;
        for (int i = 1; i < LM_GGML_MAX_DIMS; ++i) {
            nbytes += (tensor->ne[i] - 1)*tensor->nb[i];
        }
    }

    return nbytes;
}

size_t lm_ggml_nbytes_pad(const struct lm_ggml_tensor * tensor) {
    return LM_GGML_PAD(lm_ggml_nbytes(tensor), LM_GGML_MEM_ALIGN);
}

size_t lm_ggml_nbytes_split(const struct lm_ggml_tensor * tensor, int nrows_split) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return (nrows_split*tensor->ne[0]*lm_ggml_type_size(tensor->type))/lm_ggml_blck_size(tensor->type);
}

int lm_ggml_blck_size(enum lm_ggml_type type) {
    return type_traits[type].blck_size;
}

size_t lm_ggml_type_size(enum lm_ggml_type type) {
    return type_traits[type].type_size;
}

float lm_ggml_type_sizef(enum lm_ggml_type type) {
    return ((float)(type_traits[type].type_size))/type_traits[type].blck_size;
}

const char * lm_ggml_type_name(enum lm_ggml_type type) {
    return type_traits[type].type_name;
}

bool lm_ggml_is_quantized(enum lm_ggml_type type) {
    return type_traits[type].is_quantized;
}

const char * lm_ggml_op_name(enum lm_ggml_op op) {
    return LM_GGML_OP_NAME[op];
}

const char * lm_ggml_op_symbol(enum lm_ggml_op op) {
    return LM_GGML_OP_SYMBOL[op];
}

size_t lm_ggml_element_size(const struct lm_ggml_tensor * tensor) {
    return lm_ggml_type_size(tensor->type);
}

static inline bool lm_ggml_is_scalar(const struct lm_ggml_tensor * tensor) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return tensor->ne[0] == 1 && tensor->ne[1] == 1 && tensor->ne[2] == 1 && tensor->ne[3] == 1;
}

static inline bool lm_ggml_is_vector(const struct lm_ggml_tensor * tensor) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return tensor->ne[1] == 1 && tensor->ne[2] == 1 && tensor->ne[3] == 1;
}

static inline bool lm_ggml_is_matrix(const struct lm_ggml_tensor * tensor) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return tensor->ne[2] == 1 && tensor->ne[3] == 1;
}

static inline bool lm_ggml_can_mul_mat(const struct lm_ggml_tensor * t0, const struct lm_ggml_tensor * t1) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return (t0->ne[0]           == t1->ne[0])  &&
           (t1->ne[2]%t0->ne[2] == 0)          && // verify t0 is broadcastable
           (t1->ne[3]%t0->ne[3] == 0);
}

static inline bool lm_ggml_can_out_prod(const struct lm_ggml_tensor * t0, const struct lm_ggml_tensor * t1) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return (t0->ne[1] == t1->ne[1])   &&
           (t1->ne[2]%t0->ne[2] == 0) && // verify t0 is broadcastable
           (t1->ne[3]%t0->ne[3] == 0);
}

enum lm_ggml_type lm_ggml_ftype_to_lm_ggml_type(enum lm_ggml_ftype ftype) {
    enum lm_ggml_type wtype = LM_GGML_TYPE_COUNT;

    switch (ftype) {
        case LM_GGML_FTYPE_ALL_F32:              wtype = LM_GGML_TYPE_F32;   break;
        case LM_GGML_FTYPE_MOSTLY_F16:           wtype = LM_GGML_TYPE_F16;   break;
        case LM_GGML_FTYPE_MOSTLY_Q4_0:          wtype = LM_GGML_TYPE_Q4_0;  break;
        case LM_GGML_FTYPE_MOSTLY_Q4_1:          wtype = LM_GGML_TYPE_Q4_1;  break;
        case LM_GGML_FTYPE_MOSTLY_Q5_0:          wtype = LM_GGML_TYPE_Q5_0;  break;
        case LM_GGML_FTYPE_MOSTLY_Q5_1:          wtype = LM_GGML_TYPE_Q5_1;  break;
        case LM_GGML_FTYPE_MOSTLY_Q8_0:          wtype = LM_GGML_TYPE_Q8_0;  break;
        case LM_GGML_FTYPE_MOSTLY_Q2_K:          wtype = LM_GGML_TYPE_Q2_K;  break;
        case LM_GGML_FTYPE_MOSTLY_Q3_K:          wtype = LM_GGML_TYPE_Q3_K;  break;
        case LM_GGML_FTYPE_MOSTLY_Q4_K:          wtype = LM_GGML_TYPE_Q4_K;  break;
        case LM_GGML_FTYPE_MOSTLY_Q5_K:          wtype = LM_GGML_TYPE_Q5_K;  break;
        case LM_GGML_FTYPE_MOSTLY_Q6_K:          wtype = LM_GGML_TYPE_Q6_K;  break;
        case LM_GGML_FTYPE_UNKNOWN:              wtype = LM_GGML_TYPE_COUNT; break;
        case LM_GGML_FTYPE_MOSTLY_Q4_1_SOME_F16: wtype = LM_GGML_TYPE_COUNT; break;
    }

    LM_GGML_ASSERT(wtype != LM_GGML_TYPE_COUNT);

    return wtype;
}

size_t lm_ggml_tensor_overhead(void) {
    return LM_GGML_OBJECT_SIZE + LM_GGML_TENSOR_SIZE;
}

bool lm_ggml_is_transposed(const struct lm_ggml_tensor * tensor) {
    return tensor->nb[0] > tensor->nb[1];
}

bool lm_ggml_is_contiguous(const struct lm_ggml_tensor * tensor) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return
        tensor->nb[0] == lm_ggml_type_size(tensor->type) &&
        tensor->nb[1] == (tensor->nb[0]*tensor->ne[0])/lm_ggml_blck_size(tensor->type) &&
        tensor->nb[2] == tensor->nb[1]*tensor->ne[1] &&
        tensor->nb[3] == tensor->nb[2]*tensor->ne[2];
}

static inline bool lm_ggml_is_contiguous_except_dim_1(const struct lm_ggml_tensor * tensor) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return
        tensor->nb[0] == lm_ggml_type_size(tensor->type) &&
        tensor->nb[2] == tensor->nb[1]*tensor->ne[1] &&
        tensor->nb[3] == tensor->nb[2]*tensor->ne[2];
}

bool lm_ggml_is_permuted(const struct lm_ggml_tensor * tensor) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return tensor->nb[0] > tensor->nb[1] || tensor->nb[1] > tensor->nb[2] || tensor->nb[2] > tensor->nb[3];
}

static inline bool lm_ggml_is_padded_1d(const struct lm_ggml_tensor * tensor) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return
        tensor->nb[0] == lm_ggml_type_size(tensor->type) &&
        tensor->nb[2] == tensor->nb[1]*tensor->ne[1] &&
        tensor->nb[3] == tensor->nb[2]*tensor->ne[2];
}

bool lm_ggml_are_same_shape(const struct lm_ggml_tensor * t0, const struct lm_ggml_tensor * t1) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return
        (t0->ne[0] == t1->ne[0] ) &&
        (t0->ne[1] == t1->ne[1] ) &&
        (t0->ne[2] == t1->ne[2] ) &&
        (t0->ne[3] == t1->ne[3] );
}

// check if t1 can be represented as a repeatition of t0
static inline bool lm_ggml_can_repeat(const struct lm_ggml_tensor * t0, const struct lm_ggml_tensor * t1) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return
        (t1->ne[0]%t0->ne[0] == 0) &&
        (t1->ne[1]%t0->ne[1] == 0) &&
        (t1->ne[2]%t0->ne[2] == 0) &&
        (t1->ne[3]%t0->ne[3] == 0);
}

static inline bool lm_ggml_can_repeat_rows(const struct lm_ggml_tensor * t0, const struct lm_ggml_tensor * t1) {
    static_assert(LM_GGML_MAX_DIMS == 4, "LM_GGML_MAX_DIMS is not 4 - update this function");

    return (t0->ne[0] == t1->ne[0]) && lm_ggml_can_repeat(t0, t1);
}

static inline int lm_ggml_up32(int n) {
    return (n + 31) & ~31;
}

//static inline int lm_ggml_up64(int n) {
//    return (n + 63) & ~63;
//}

static inline int lm_ggml_up(int n, int m) {
    // assert m is a power of 2
    LM_GGML_ASSERT((m & (m - 1)) == 0);
    return (n + m - 1) & ~(m - 1);
}

// assert that pointer is aligned to LM_GGML_MEM_ALIGN
#define lm_ggml_assert_aligned(ptr) \
    LM_GGML_ASSERT(((uintptr_t) (ptr))%LM_GGML_MEM_ALIGN == 0)

////////////////////////////////////////////////////////////////////////////////

struct lm_ggml_context * lm_ggml_init(struct lm_ggml_init_params params) {
    // make this function thread safe
    lm_ggml_critical_section_start();

    static bool is_first_call = true;

    if (is_first_call) {
        // initialize time system (required on Windows)
        lm_ggml_time_init();

        // initialize GELU, Quick GELU, SILU and EXP F32 tables
        {
            const uint64_t t_start = lm_ggml_time_us(); UNUSED(t_start);

            lm_ggml_fp16_t ii;
            for (int i = 0; i < (1 << 16); ++i) {
                uint16_t ui = i;
                memcpy(&ii, &ui, sizeof(ii));
                const float f = table_f32_f16[i] = LM_GGML_COMPUTE_FP16_TO_FP32(ii);
                table_gelu_f16[i] = LM_GGML_FP32_TO_FP16(lm_ggml_gelu_f32(f));
                table_gelu_quick_f16[i] = LM_GGML_FP32_TO_FP16(lm_ggml_gelu_quick_f32(f));
                table_silu_f16[i] = LM_GGML_FP32_TO_FP16(lm_ggml_silu_f32(f));
                table_exp_f16[i]  = LM_GGML_FP32_TO_FP16(expf(f));
            }

            const uint64_t t_end = lm_ggml_time_us(); UNUSED(t_end);

            LM_GGML_PRINT_DEBUG("%s: GELU, Quick GELU, SILU and EXP tables initialized in %f ms\n", __func__, (t_end - t_start)/1000.0f);
        }

        // initialize g_state
        {
            const uint64_t t_start = lm_ggml_time_us(); UNUSED(t_start);

            g_state = (struct lm_ggml_state) {
                /*.contexts =*/ { { 0 } },
                /*.numa =*/ {
                    .n_nodes = 0,
                    .total_cpus = 0,
                },
            };

            for (int i = 0; i < LM_GGML_MAX_CONTEXTS; ++i) {
                g_state.contexts[i].used = false;
            }

            const uint64_t t_end = lm_ggml_time_us(); UNUSED(t_end);

            LM_GGML_PRINT_DEBUG("%s: g_state initialized in %f ms\n", __func__, (t_end - t_start)/1000.0f);
        }

#if defined(LM_GGML_USE_CUBLAS)
        lm_ggml_init_cublas();
#elif defined(LM_GGML_USE_CLBLAST)
        lm_ggml_cl_init();
#endif

        lm_ggml_setup_op_has_task_pass();

        is_first_call = false;
    }

    // find non-used context in g_state
    struct lm_ggml_context * ctx = NULL;

    for (int i = 0; i < LM_GGML_MAX_CONTEXTS; i++) {
        if (!g_state.contexts[i].used) {
            g_state.contexts[i].used = true;
            ctx = &g_state.contexts[i].context;

            LM_GGML_PRINT_DEBUG("%s: found unused context %d\n", __func__, i);
            break;
        }
    }

    if (ctx == NULL) {
        LM_GGML_PRINT_DEBUG("%s: no unused context found\n", __func__);

        lm_ggml_critical_section_end();

        return NULL;
    }

    // allow to call lm_ggml_init with 0 size
    if (params.mem_size == 0) {
        params.mem_size = LM_GGML_MEM_ALIGN;
    }

    const size_t mem_size = params.mem_buffer ? params.mem_size : LM_GGML_PAD(params.mem_size, LM_GGML_MEM_ALIGN);

    *ctx = (struct lm_ggml_context) {
        /*.mem_size           =*/ mem_size,
        /*.mem_buffer         =*/ params.mem_buffer ? params.mem_buffer : LM_GGML_ALIGNED_MALLOC(mem_size),
        /*.mem_buffer_owned   =*/ params.mem_buffer ? false : true,
        /*.no_alloc           =*/ params.no_alloc,
        /*.no_alloc_save      =*/ params.no_alloc,
        /*.n_objects          =*/ 0,
        /*.objects_begin      =*/ NULL,
        /*.objects_end        =*/ NULL,
        /*.scratch            =*/ { 0, 0, NULL, },
        /*.scratch_save       =*/ { 0, 0, NULL, },
    };

    LM_GGML_ASSERT(ctx->mem_buffer != NULL);

    lm_ggml_assert_aligned(ctx->mem_buffer);

    LM_GGML_PRINT_DEBUG("%s: context initialized\n", __func__);

    lm_ggml_critical_section_end();

    return ctx;
}

void lm_ggml_free(struct lm_ggml_context * ctx) {
    // make this function thread safe
    lm_ggml_critical_section_start();

    bool found = false;

    for (int i = 0; i < LM_GGML_MAX_CONTEXTS; i++) {
        if (&g_state.contexts[i].context == ctx) {
            g_state.contexts[i].used = false;

            LM_GGML_PRINT_DEBUG("%s: context %d has been freed. memory used = %zu\n",
                    __func__, i, lm_ggml_used_mem(ctx));

            if (ctx->mem_buffer_owned) {
                LM_GGML_ALIGNED_FREE(ctx->mem_buffer);
            }

            found = true;
            break;
        }
    }

    if (!found) {
        LM_GGML_PRINT_DEBUG("%s: context not found\n", __func__);
    }

    lm_ggml_critical_section_end();
}

size_t lm_ggml_used_mem(const struct lm_ggml_context * ctx) {
    return ctx->objects_end == NULL ? 0 : ctx->objects_end->offs + ctx->objects_end->size;
}

size_t lm_ggml_set_scratch(struct lm_ggml_context * ctx, struct lm_ggml_scratch scratch) {
    const size_t result = ctx->scratch.data ? ctx->scratch.offs : 0;

    ctx->scratch = scratch;

    return result;
}

bool lm_ggml_get_no_alloc(struct lm_ggml_context * ctx) {
    return ctx->no_alloc;
}

void lm_ggml_set_no_alloc(struct lm_ggml_context * ctx, bool no_alloc) {
    ctx->no_alloc = no_alloc;
}

void * lm_ggml_get_mem_buffer(const struct lm_ggml_context * ctx) {
    return ctx->mem_buffer;
}

size_t lm_ggml_get_mem_size(const struct lm_ggml_context * ctx) {
    return ctx->mem_size;
}

size_t lm_ggml_get_max_tensor_size(const struct lm_ggml_context * ctx) {
    size_t max_size = 0;

    struct lm_ggml_object * obj = ctx->objects_begin;

    while (obj != NULL) {
        if (obj->type == LM_GGML_OBJECT_TENSOR) {
            struct lm_ggml_tensor * tensor = (struct lm_ggml_tensor *) ((char *) ctx->mem_buffer + obj->offs);

            const size_t size = lm_ggml_nbytes(tensor);

            if (max_size < size) {
                max_size = size;
            }
        }

        obj = obj->next;
    }

    return max_size;
}

// IMPORTANT:
// when creating "opt" tensors, always save and load the scratch buffer
// this is an error prone process, but it is necessary to support inplace
// operators when using scratch buffers
// TODO: implement a better way
static void lm_ggml_scratch_save(struct lm_ggml_context * ctx) {
    // this is needed to allow opt tensors to store their data
    // TODO: again, need to find a better way
    ctx->no_alloc_save = ctx->no_alloc;
    ctx->no_alloc      = false;

    ctx->scratch_save = ctx->scratch;
    ctx->scratch.data = NULL;
}

static void lm_ggml_scratch_load(struct lm_ggml_context * ctx) {
    ctx->no_alloc = ctx->no_alloc_save;

    ctx->scratch = ctx->scratch_save;
}

////////////////////////////////////////////////////////////////////////////////

static struct lm_ggml_object * lm_ggml_new_object(struct lm_ggml_context * ctx, enum lm_ggml_object_type type, size_t size) {
    // always insert objects at the end of the context's memory pool
    struct lm_ggml_object * obj_cur = ctx->objects_end;

    const size_t cur_offs = obj_cur == NULL ? 0 : obj_cur->offs;
    const size_t cur_size = obj_cur == NULL ? 0 : obj_cur->size;
    const size_t cur_end  = cur_offs + cur_size;

    // align to LM_GGML_MEM_ALIGN
    size_t size_needed = LM_GGML_PAD(size, LM_GGML_MEM_ALIGN);

    char * const mem_buffer = ctx->mem_buffer;
    struct lm_ggml_object * const obj_new = (struct lm_ggml_object *)(mem_buffer + cur_end);

    if (cur_end + size_needed + LM_GGML_OBJECT_SIZE > ctx->mem_size) {
        LM_GGML_PRINT("%s: not enough space in the context's memory pool (needed %zu, available %zu)\n",
                __func__, cur_end + size_needed, ctx->mem_size);
        assert(false);
        return NULL;
    }

    *obj_new = (struct lm_ggml_object) {
        .offs = cur_end + LM_GGML_OBJECT_SIZE,
        .size = size_needed,
        .next = NULL,
        .type = type,
    };

    lm_ggml_assert_aligned(mem_buffer + obj_new->offs);

    if (obj_cur != NULL) {
        obj_cur->next = obj_new;
    } else {
        // this is the first object in this context
        ctx->objects_begin = obj_new;
    }

    ctx->objects_end = obj_new;

    //printf("%s: inserted new object at %zu, size = %zu\n", __func__, cur_end, obj_new->size);

    return obj_new;
}

static struct lm_ggml_tensor * lm_ggml_new_tensor_impl(
        struct lm_ggml_context * ctx,
        enum   lm_ggml_type      type,
        int                   n_dims,
        const int64_t       * ne,
        struct lm_ggml_tensor  * view_src,
        size_t                view_offs) {

    assert(n_dims >= 1 && n_dims <= LM_GGML_MAX_DIMS);

    // find the base tensor and absolute offset
    if (view_src != NULL && view_src->view_src != NULL) {
        view_offs += view_src->view_offs;
        view_src   = view_src->view_src;
    }

    size_t data_size = lm_ggml_type_size(type)*(ne[0]/lm_ggml_blck_size(type));
    for (int i = 1; i < n_dims; i++) {
        data_size *= ne[i];
    }

    LM_GGML_ASSERT(view_src == NULL || data_size + view_offs <= lm_ggml_nbytes(view_src));

    void * data = view_src != NULL ? view_src->data : NULL;
    if (data != NULL) {
        data = (char *) data + view_offs;
    }

    size_t obj_alloc_size = 0;

    if (view_src == NULL && !ctx->no_alloc) {
        if (ctx->scratch.data != NULL) {
            // allocate tensor data in the scratch buffer
            if (ctx->scratch.offs + data_size > ctx->scratch.size) {
                LM_GGML_PRINT("%s: not enough space in the scratch memory pool (needed %zu, available %zu)\n",
                        __func__, ctx->scratch.offs + data_size, ctx->scratch.size);
                assert(false);
                return NULL;
            }

            data = (char * const) ctx->scratch.data + ctx->scratch.offs;

            ctx->scratch.offs += data_size;
        } else {
            // allocate tensor data in the context's memory pool
            obj_alloc_size = data_size;
        }
    }

    struct lm_ggml_object * const obj_new = lm_ggml_new_object(ctx, LM_GGML_OBJECT_TENSOR, LM_GGML_TENSOR_SIZE + obj_alloc_size);

    // TODO: for recoverable errors, we would need to free the data allocated from the scratch buffer here

    struct lm_ggml_tensor * const result = (struct lm_ggml_tensor *)((char *)ctx->mem_buffer + obj_new->offs);

    *result = (struct lm_ggml_tensor) {
        /*.type         =*/ type,
        /*.backend      =*/ LM_GGML_BACKEND_CPU,
        /*.buffer       =*/ NULL,
        /*.n_dims       =*/ n_dims,
        /*.ne           =*/ { 1, 1, 1, 1 },
        /*.nb           =*/ { 0, 0, 0, 0 },
        /*.op           =*/ LM_GGML_OP_NONE,
        /*.op_params    =*/ { 0 },
        /*.is_param     =*/ false,
        /*.grad         =*/ NULL,
        /*.src          =*/ { NULL },
        /*.perf_runs    =*/ 0,
        /*.perf_cycles  =*/ 0,
        /*.perf_time_us =*/ 0,
        /*.view_src     =*/ view_src,
        /*.view_offs    =*/ view_offs,
        /*.data         =*/ obj_alloc_size > 0 ? (void *)(result + 1) : data,
        /*.name         =*/ { 0 },
        /*.extra        =*/ NULL,
        /*.padding      =*/ { 0 },
    };

    // TODO: this should not be needed as long as we don't rely on aligned SIMD loads
    //lm_ggml_assert_aligned(result->data);

    for (int i = 0; i < n_dims; i++) {
        result->ne[i] = ne[i];
    }

    result->nb[0] = lm_ggml_type_size(type);
    result->nb[1] = result->nb[0]*(result->ne[0]/lm_ggml_blck_size(type));
    for (int i = 2; i < LM_GGML_MAX_DIMS; i++) {
        result->nb[i] = result->nb[i - 1]*result->ne[i - 1];
    }

    ctx->n_objects++;

    return result;
}

struct lm_ggml_tensor * lm_ggml_new_tensor(
        struct lm_ggml_context * ctx,
        enum   lm_ggml_type      type,
        int                   n_dims,
        const int64_t       * ne) {
    return lm_ggml_new_tensor_impl(ctx, type, n_dims, ne, NULL, 0);
}

struct lm_ggml_tensor * lm_ggml_new_tensor_1d(
        struct lm_ggml_context * ctx,
        enum   lm_ggml_type      type,
        int64_t ne0) {
    return lm_ggml_new_tensor(ctx, type, 1, &ne0);
}

struct lm_ggml_tensor * lm_ggml_new_tensor_2d(
        struct lm_ggml_context * ctx,
        enum   lm_ggml_type      type,
        int64_t ne0,
        int64_t ne1) {
    const int64_t ne[2] = { ne0, ne1 };
    return lm_ggml_new_tensor(ctx, type, 2, ne);
}

struct lm_ggml_tensor * lm_ggml_new_tensor_3d(
        struct lm_ggml_context * ctx,
        enum   lm_ggml_type      type,
        int64_t ne0,
        int64_t ne1,
        int64_t ne2) {
    const int64_t ne[3] = { ne0, ne1, ne2 };
    return lm_ggml_new_tensor(ctx, type, 3, ne);
}

struct lm_ggml_tensor * lm_ggml_new_tensor_4d(
        struct lm_ggml_context * ctx,
        enum   lm_ggml_type type,
        int64_t ne0,
        int64_t ne1,
        int64_t ne2,
        int64_t ne3) {
    const int64_t ne[4] = { ne0, ne1, ne2, ne3 };
    return lm_ggml_new_tensor(ctx, type, 4, ne);
}

struct lm_ggml_tensor * lm_ggml_new_i32(struct lm_ggml_context * ctx, int32_t value) {
    lm_ggml_scratch_save(ctx);

    struct lm_ggml_tensor * result = lm_ggml_new_tensor_1d(ctx, LM_GGML_TYPE_I32, 1);

    lm_ggml_scratch_load(ctx);

    lm_ggml_set_i32(result, value);

    return result;
}

struct lm_ggml_tensor * lm_ggml_new_f32(struct lm_ggml_context * ctx, float value) {
    lm_ggml_scratch_save(ctx);

    struct lm_ggml_tensor * result = lm_ggml_new_tensor_1d(ctx, LM_GGML_TYPE_F32, 1);

    lm_ggml_scratch_load(ctx);

    lm_ggml_set_f32(result, value);

    return result;
}

struct lm_ggml_tensor * lm_ggml_dup_tensor(struct lm_ggml_context * ctx, const struct lm_ggml_tensor * src) {
    return lm_ggml_new_tensor(ctx, src->type, src->n_dims, src->ne);
}

static void lm_ggml_set_op_params(struct lm_ggml_tensor * tensor, const void * params, size_t params_size) {
    LM_GGML_ASSERT(tensor != NULL); // silence -Warray-bounds warnings
    assert(params_size <= LM_GGML_MAX_OP_PARAMS);
    memcpy(tensor->op_params, params, params_size);
}

static int32_t lm_ggml_get_op_params_i32(const struct lm_ggml_tensor * tensor, uint32_t i) {
    assert(i < LM_GGML_MAX_OP_PARAMS / sizeof(int32_t));
    return ((const int32_t *)(tensor->op_params))[i];
}

static void lm_ggml_set_op_params_i32(struct lm_ggml_tensor * tensor, uint32_t i, int32_t value) {
    assert(i < LM_GGML_MAX_OP_PARAMS / sizeof(int32_t));
    ((int32_t *)(tensor->op_params))[i] = value;
}

struct lm_ggml_tensor * lm_ggml_set_zero(struct lm_ggml_tensor * tensor) {
    memset(tensor->data, 0, lm_ggml_nbytes(tensor));
    return tensor;
}

struct lm_ggml_tensor * lm_ggml_set_i32 (struct lm_ggml_tensor * tensor, int32_t value) {
    const int n     = lm_ggml_nrows(tensor);
    const int nc    = tensor->ne[0];
    const size_t n1 = tensor->nb[1];

    char * const data = tensor->data;

    switch (tensor->type) {
        case LM_GGML_TYPE_I8:
            {
                assert(tensor->nb[0] == sizeof(int8_t));
                for (int i = 0; i < n; i++) {
                    lm_ggml_vec_set_i8(nc, (int8_t *)(data + i*n1), value);
                }
            } break;
        case LM_GGML_TYPE_I16:
            {
                assert(tensor->nb[0] == sizeof(int16_t));
                for (int i = 0; i < n; i++) {
                    lm_ggml_vec_set_i16(nc, (int16_t *)(data + i*n1), value);
                }
            } break;
        case LM_GGML_TYPE_I32:
            {
                assert(tensor->nb[0] == sizeof(int32_t));
                for (int i = 0; i < n; i++) {
                    lm_ggml_vec_set_i32(nc, (int32_t *)(data + i*n1), value);
                }
            } break;
        case LM_GGML_TYPE_F16:
            {
                assert(tensor->nb[0] == sizeof(lm_ggml_fp16_t));
                for (int i = 0; i < n; i++) {
                    lm_ggml_vec_set_f16(nc, (lm_ggml_fp16_t *)(data + i*n1), LM_GGML_FP32_TO_FP16(value));
                }
            } break;
        case LM_GGML_TYPE_F32:
            {
                assert(tensor->nb[0] == sizeof(float));
                for (int i = 0; i < n; i++) {
                    lm_ggml_vec_set_f32(nc, (float *)(data + i*n1), value);
                }
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }

    return tensor;
}

struct lm_ggml_tensor * lm_ggml_set_f32(struct lm_ggml_tensor * tensor, float value) {
    const int n     = lm_ggml_nrows(tensor);
    const int nc    = tensor->ne[0];
    const size_t n1 = tensor->nb[1];

    char * const data = tensor->data;

    switch (tensor->type) {
        case LM_GGML_TYPE_I8:
            {
                assert(tensor->nb[0] == sizeof(int8_t));
                for (int i = 0; i < n; i++) {
                    lm_ggml_vec_set_i8(nc, (int8_t *)(data + i*n1), value);
                }
            } break;
        case LM_GGML_TYPE_I16:
            {
                assert(tensor->nb[0] == sizeof(int16_t));
                for (int i = 0; i < n; i++) {
                    lm_ggml_vec_set_i16(nc, (int16_t *)(data + i*n1), value);
                }
            } break;
        case LM_GGML_TYPE_I32:
            {
                assert(tensor->nb[0] == sizeof(int32_t));
                for (int i = 0; i < n; i++) {
                    lm_ggml_vec_set_i32(nc, (int32_t *)(data + i*n1), value);
                }
            } break;
        case LM_GGML_TYPE_F16:
            {
                assert(tensor->nb[0] == sizeof(lm_ggml_fp16_t));
                for (int i = 0; i < n; i++) {
                    lm_ggml_vec_set_f16(nc, (lm_ggml_fp16_t *)(data + i*n1), LM_GGML_FP32_TO_FP16(value));
                }
            } break;
        case LM_GGML_TYPE_F32:
            {
                assert(tensor->nb[0] == sizeof(float));
                for (int i = 0; i < n; i++) {
                    lm_ggml_vec_set_f32(nc, (float *)(data + i*n1), value);
                }
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }

    return tensor;
}

void lm_ggml_unravel_index(const struct lm_ggml_tensor * tensor, int64_t i, int64_t * i0, int64_t * i1, int64_t * i2, int64_t * i3) {
    const int64_t ne2 = tensor->ne[2];
    const int64_t ne1 = tensor->ne[1];
    const int64_t ne0 = tensor->ne[0];

    const int64_t i3_ = (i/(ne2*ne1*ne0));
    const int64_t i2_ = (i - i3_*ne2*ne1*ne0)/(ne1*ne0);
    const int64_t i1_ = (i - i3_*ne2*ne1*ne0 - i2_*ne1*ne0)/ne0;
    const int64_t i0_ = (i - i3_*ne2*ne1*ne0 - i2_*ne1*ne0 - i1_*ne0);

    if (i0) {
        * i0 = i0_;
    }
    if (i1) {
        * i1 = i1_;
    }
    if (i2) {
        * i2 = i2_;
    }
    if (i3) {
        * i3 = i3_;
    }
}

int32_t lm_ggml_get_i32_1d(const struct lm_ggml_tensor * tensor, int i) {
    if (!lm_ggml_is_contiguous(tensor)) {
        int64_t id[4] = { 0, 0, 0, 0 };
        lm_ggml_unravel_index(tensor, i, &id[0], &id[1], &id[2], &id[3]);
        return lm_ggml_get_i32_nd(tensor, id[0], id[1], id[2], id[3]);
    }
    switch (tensor->type) {
        case LM_GGML_TYPE_I8:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(int8_t));
                return ((int8_t *)(tensor->data))[i];
            }
        case LM_GGML_TYPE_I16:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(int16_t));
                return ((int16_t *)(tensor->data))[i];
            }
        case LM_GGML_TYPE_I32:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(int32_t));
                return ((int32_t *)(tensor->data))[i];
            }
        case LM_GGML_TYPE_F16:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(lm_ggml_fp16_t));
                return LM_GGML_FP16_TO_FP32(((lm_ggml_fp16_t *)(tensor->data))[i]);
            }
        case LM_GGML_TYPE_F32:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(float));
                return ((float *)(tensor->data))[i];
            }
        default:
            {
                LM_GGML_ASSERT(false);
            }
    }

    return 0.0f;
}

void lm_ggml_set_i32_1d(const struct lm_ggml_tensor * tensor, int i, int32_t value) {
    if (!lm_ggml_is_contiguous(tensor)) {
        int64_t id[4] = { 0, 0, 0, 0 };
        lm_ggml_unravel_index(tensor, i, &id[0], &id[1], &id[2], &id[3]);
        lm_ggml_set_i32_nd(tensor, id[0], id[1], id[2], id[3], value);
        return;
    }
    switch (tensor->type) {
        case LM_GGML_TYPE_I8:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(int8_t));
                ((int8_t *)(tensor->data))[i] = value;
            } break;
        case LM_GGML_TYPE_I16:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(int16_t));
                ((int16_t *)(tensor->data))[i] = value;
            } break;
        case LM_GGML_TYPE_I32:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(int32_t));
                ((int32_t *)(tensor->data))[i] = value;
            } break;
        case LM_GGML_TYPE_F16:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(lm_ggml_fp16_t));
                ((lm_ggml_fp16_t *)(tensor->data))[i] = LM_GGML_FP32_TO_FP16(value);
            } break;
        case LM_GGML_TYPE_F32:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(float));
                ((float *)(tensor->data))[i] = value;
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

int32_t lm_ggml_get_i32_nd(const struct lm_ggml_tensor * tensor, int i0, int i1, int i2, int i3) {
    void * data   = (char *) tensor->data + i0*tensor->nb[0] + i1*tensor->nb[1] + i2*tensor->nb[2] + i3*tensor->nb[3];
    switch (tensor->type) {
        case LM_GGML_TYPE_I8:
            return ((int8_t *) data)[0];
        case LM_GGML_TYPE_I16:
            return ((int16_t *) data)[0];
        case LM_GGML_TYPE_I32:
            return ((int32_t *) data)[0];
        case LM_GGML_TYPE_F16:
            return LM_GGML_FP16_TO_FP32(((lm_ggml_fp16_t *) data)[0]);
        case LM_GGML_TYPE_F32:
            return ((float *) data)[0];
        default:
            LM_GGML_ASSERT(false);
    }

    return 0.0f;
}

void lm_ggml_set_i32_nd(const struct lm_ggml_tensor * tensor, int i0, int i1, int i2, int i3, int32_t value) {
    void * data   = (char *) tensor->data + i0*tensor->nb[0] + i1*tensor->nb[1] + i2*tensor->nb[2] + i3*tensor->nb[3];
    switch (tensor->type) {
        case LM_GGML_TYPE_I8:
            {
                ((int8_t *)(data))[0] = value;
            } break;
        case LM_GGML_TYPE_I16:
            {
                ((int16_t *)(data))[0] = value;
            } break;
        case LM_GGML_TYPE_I32:
            {
                ((int32_t *)(data))[0] = value;
            } break;
        case LM_GGML_TYPE_F16:
            {
                ((lm_ggml_fp16_t *)(data))[0] = LM_GGML_FP32_TO_FP16(value);
            } break;
        case LM_GGML_TYPE_F32:
            {
                ((float *)(data))[0] = value;
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

float lm_ggml_get_f32_1d(const struct lm_ggml_tensor * tensor, int i) {
    if (!lm_ggml_is_contiguous(tensor)) {
        int64_t id[4] = { 0, 0, 0, 0 };
        lm_ggml_unravel_index(tensor, i, &id[0], &id[1], &id[2], &id[3]);
        return lm_ggml_get_f32_nd(tensor, id[0], id[1], id[2], id[3]);
    }
    switch (tensor->type) {
        case LM_GGML_TYPE_I8:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(int8_t));
                return ((int8_t *)(tensor->data))[i];
            }
        case LM_GGML_TYPE_I16:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(int16_t));
                return ((int16_t *)(tensor->data))[i];
            }
        case LM_GGML_TYPE_I32:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(int32_t));
                return ((int32_t *)(tensor->data))[i];
            }
        case LM_GGML_TYPE_F16:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(lm_ggml_fp16_t));
                return LM_GGML_FP16_TO_FP32(((lm_ggml_fp16_t *)(tensor->data))[i]);
            }
        case LM_GGML_TYPE_F32:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(float));
                return ((float *)(tensor->data))[i];
            }
        default:
            {
                LM_GGML_ASSERT(false);
            }
    }

    return 0.0f;
}

void lm_ggml_set_f32_1d(const struct lm_ggml_tensor * tensor, int i, float value) {
    if (!lm_ggml_is_contiguous(tensor)) {
        int64_t id[4] = { 0, 0, 0, 0 };
        lm_ggml_unravel_index(tensor, i, &id[0], &id[1], &id[2], &id[3]);
        lm_ggml_set_f32_nd(tensor, id[0], id[1], id[2], id[3], value);
        return;
    }
    switch (tensor->type) {
        case LM_GGML_TYPE_I8:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(int8_t));
                ((int8_t *)(tensor->data))[i] = value;
            } break;
        case LM_GGML_TYPE_I16:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(int16_t));
                ((int16_t *)(tensor->data))[i] = value;
            } break;
        case LM_GGML_TYPE_I32:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(int32_t));
                ((int32_t *)(tensor->data))[i] = value;
            } break;
        case LM_GGML_TYPE_F16:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(lm_ggml_fp16_t));
                ((lm_ggml_fp16_t *)(tensor->data))[i] = LM_GGML_FP32_TO_FP16(value);
            } break;
        case LM_GGML_TYPE_F32:
            {
                LM_GGML_ASSERT(tensor->nb[0] == sizeof(float));
                ((float *)(tensor->data))[i] = value;
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

float lm_ggml_get_f32_nd(const struct lm_ggml_tensor * tensor, int i0, int i1, int i2, int i3) {
    void * data   = (char *) tensor->data + i0*tensor->nb[0] + i1*tensor->nb[1] + i2*tensor->nb[2] + i3*tensor->nb[3];
    switch (tensor->type) {
        case LM_GGML_TYPE_I8:
            return ((int8_t *) data)[0];
        case LM_GGML_TYPE_I16:
            return ((int16_t *) data)[0];
        case LM_GGML_TYPE_I32:
            return ((int32_t *) data)[0];
        case LM_GGML_TYPE_F16:
            return LM_GGML_FP16_TO_FP32(((lm_ggml_fp16_t *) data)[0]);
        case LM_GGML_TYPE_F32:
            return ((float *) data)[0];
        default:
            LM_GGML_ASSERT(false);
    }

    return 0.0f;
}

void lm_ggml_set_f32_nd(const struct lm_ggml_tensor * tensor, int i0, int i1, int i2, int i3, float value) {
    void * data   = (char *) tensor->data + i0*tensor->nb[0] + i1*tensor->nb[1] + i2*tensor->nb[2] + i3*tensor->nb[3];
    switch (tensor->type) {
        case LM_GGML_TYPE_I8:
            {
                ((int8_t *)(data))[0] = value;
            } break;
        case LM_GGML_TYPE_I16:
            {
                ((int16_t *)(data))[0] = value;
            } break;
        case LM_GGML_TYPE_I32:
            {
                ((int32_t *)(data))[0] = value;
            } break;
        case LM_GGML_TYPE_F16:
            {
                ((lm_ggml_fp16_t *)(data))[0] = LM_GGML_FP32_TO_FP16(value);
            } break;
        case LM_GGML_TYPE_F32:
            {
                ((float *)(data))[0] = value;
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

void * lm_ggml_get_data(const struct lm_ggml_tensor * tensor) {
    return tensor->data;
}

float * lm_ggml_get_data_f32(const struct lm_ggml_tensor * tensor) {
    assert(tensor->type == LM_GGML_TYPE_F32);
    return (float *)(tensor->data);
}

enum lm_ggml_unary_op lm_ggml_get_unary_op(const struct lm_ggml_tensor * tensor) {
    LM_GGML_ASSERT(tensor->op == LM_GGML_OP_UNARY);
    return (enum lm_ggml_unary_op) lm_ggml_get_op_params_i32(tensor, 0);
}

const char * lm_ggml_get_name(const struct lm_ggml_tensor * tensor) {
    return tensor->name;
}

struct lm_ggml_tensor * lm_ggml_set_name(struct lm_ggml_tensor * tensor, const char * name) {
    strncpy(tensor->name, name, sizeof(tensor->name));
    tensor->name[sizeof(tensor->name) - 1] = '\0';
    return tensor;
}

struct lm_ggml_tensor * lm_ggml_format_name(struct lm_ggml_tensor * tensor, const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(tensor->name, sizeof(tensor->name), fmt, args);
    va_end(args);
    return tensor;
}

struct lm_ggml_tensor * lm_ggml_view_tensor(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * src) {
    struct lm_ggml_tensor * result = lm_ggml_new_tensor_impl(ctx, src->type, src->n_dims, src->ne, src, 0);
    lm_ggml_format_name(result, "%s (view)", src->name);

    for (int i = 0; i < LM_GGML_MAX_DIMS; i++) {
        result->nb[i] = src->nb[i];
    }

    return result;
}

struct lm_ggml_tensor * lm_ggml_get_first_tensor(struct lm_ggml_context * ctx) {
    struct lm_ggml_object * obj = ctx->objects_begin;

    char * const mem_buffer = ctx->mem_buffer;

    while (obj != NULL) {
        if (obj->type == LM_GGML_OBJECT_TENSOR) {
            return (struct lm_ggml_tensor *)(mem_buffer + obj->offs);
        }

        obj = obj->next;
    }

    return NULL;
}

struct lm_ggml_tensor * lm_ggml_get_next_tensor(struct lm_ggml_context * ctx, struct lm_ggml_tensor * tensor) {
    struct lm_ggml_object * obj = (struct lm_ggml_object *) ((char *)tensor - LM_GGML_OBJECT_SIZE);
    obj = obj->next;

    char * const mem_buffer = ctx->mem_buffer;

    while (obj != NULL) {
        if (obj->type == LM_GGML_OBJECT_TENSOR) {
            return (struct lm_ggml_tensor *)(mem_buffer + obj->offs);
        }

        obj = obj->next;
    }

    return NULL;
}

struct lm_ggml_tensor * lm_ggml_get_tensor(struct lm_ggml_context * ctx, const char * name) {
    struct lm_ggml_object * obj = ctx->objects_begin;

    char * const mem_buffer = ctx->mem_buffer;

    while (obj != NULL) {
        if (obj->type == LM_GGML_OBJECT_TENSOR) {
            struct lm_ggml_tensor * cur = (struct lm_ggml_tensor *)(mem_buffer + obj->offs);
            if (strcmp(cur->name, name) == 0) {
                return cur;
            }
        }

        obj = obj->next;
    }

    return NULL;
}

////////////////////////////////////////////////////////////////////////////////

// lm_ggml_dup

static struct lm_ggml_tensor * lm_ggml_dup_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        bool inplace) {
    bool is_node = false;

    if (!inplace && (a->grad)) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    result->op   = LM_GGML_OP_DUP;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_dup(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a) {
    return lm_ggml_dup_impl(ctx, a, false);
}

struct lm_ggml_tensor * lm_ggml_dup_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a) {
    return lm_ggml_dup_impl(ctx, a, true);
}

// lm_ggml_add

static struct lm_ggml_tensor * lm_ggml_add_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b,
        bool inplace) {
    // TODO: support less-strict constraint
    //       LM_GGML_ASSERT(lm_ggml_can_repeat(b, a));
    LM_GGML_ASSERT(lm_ggml_can_repeat_rows(b, a));

    bool is_node = false;

    if (!inplace && (a->grad || b->grad)) {
        // TODO: support backward pass for broadcasting
        LM_GGML_ASSERT(lm_ggml_are_same_shape(a, b));
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    result->op   = LM_GGML_OP_ADD;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_add(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b) {
    return lm_ggml_add_impl(ctx, a, b, false);
}

struct lm_ggml_tensor * lm_ggml_add_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b) {
    return lm_ggml_add_impl(ctx, a, b, true);
}

// lm_ggml_add_cast

static struct lm_ggml_tensor * lm_ggml_add_cast_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b,
        enum   lm_ggml_type     type) {
    // TODO: support less-strict constraint
    //       LM_GGML_ASSERT(lm_ggml_can_repeat(b, a));
    LM_GGML_ASSERT(lm_ggml_can_repeat_rows(b, a));
    LM_GGML_ASSERT(lm_ggml_is_quantized(a->type)); // currently only supported for quantized input

    bool is_node = false;

    if (a->grad || b->grad) {
        // TODO: support backward pass for broadcasting
        LM_GGML_ASSERT(lm_ggml_are_same_shape(a, b));
        is_node = true;
    }

    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, type, a->n_dims, a->ne);

    result->op   = LM_GGML_OP_ADD;
    result->grad = is_node ? lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, a->n_dims, a->ne) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_add_cast(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b,
        enum   lm_ggml_type     type) {
    return lm_ggml_add_cast_impl(ctx, a, b, type);
}

// lm_ggml_add1

static struct lm_ggml_tensor * lm_ggml_add1_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b,
        bool inplace) {
    LM_GGML_ASSERT(lm_ggml_is_scalar(b));
    LM_GGML_ASSERT(lm_ggml_is_padded_1d(a));

    bool is_node = false;

    if (a->grad || b->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    result->op   = LM_GGML_OP_ADD1;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_add1(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b) {
    return lm_ggml_add1_impl(ctx, a, b, false);
}

struct lm_ggml_tensor * lm_ggml_add1_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b) {
    return lm_ggml_add1_impl(ctx, a, b, true);
}

// lm_ggml_acc

static struct lm_ggml_tensor * lm_ggml_acc_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b,
        size_t               nb1,
        size_t               nb2,
        size_t               nb3,
        size_t               offset,
        bool inplace) {
    LM_GGML_ASSERT(lm_ggml_nelements(b) <= lm_ggml_nelements(a));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(a));
    LM_GGML_ASSERT(a->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT(b->type == LM_GGML_TYPE_F32);

    bool is_node = false;

    if (!inplace && (a->grad || b->grad)) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    int32_t params[] = { nb1, nb2, nb3, offset, inplace ? 1 : 0 };
    lm_ggml_set_op_params(result, params, sizeof(params));

    result->op   = LM_GGML_OP_ACC;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_acc(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b,
        size_t               nb1,
        size_t               nb2,
        size_t               nb3,
        size_t               offset) {
    return lm_ggml_acc_impl(ctx, a, b, nb1, nb2, nb3, offset, false);
}

struct lm_ggml_tensor * lm_ggml_acc_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b,
        size_t               nb1,
        size_t               nb2,
        size_t               nb3,
        size_t               offset) {
    return lm_ggml_acc_impl(ctx, a, b, nb1, nb2, nb3, offset, true);
}

// lm_ggml_sub

static struct lm_ggml_tensor * lm_ggml_sub_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b,
        bool inplace) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(a, b));

    bool is_node = false;

    if (!inplace && (a->grad || b->grad)) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    result->op   = LM_GGML_OP_SUB;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_sub(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b) {
    return lm_ggml_sub_impl(ctx, a, b, false);
}

struct lm_ggml_tensor * lm_ggml_sub_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b) {
    return lm_ggml_sub_impl(ctx, a, b, true);
}

// lm_ggml_mul

static struct lm_ggml_tensor * lm_ggml_mul_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b,
        bool inplace) {
    // TODO: support less-strict constraint
    //       LM_GGML_ASSERT(lm_ggml_can_repeat(b, a));
    LM_GGML_ASSERT(lm_ggml_can_repeat_rows(b, a));

    bool is_node = false;

    if (!inplace && (a->grad || b->grad)) {
        // TODO: support backward pass for broadcasting
        LM_GGML_ASSERT(lm_ggml_are_same_shape(a, b));
        is_node = true;
    }

    if (inplace) {
        LM_GGML_ASSERT(!is_node);
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    result->op   = LM_GGML_OP_MUL;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_mul(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b) {
    return lm_ggml_mul_impl(ctx, a, b, false);
}

struct lm_ggml_tensor * lm_ggml_mul_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b) {
    return lm_ggml_mul_impl(ctx, a, b, true);
}

// lm_ggml_div

static struct lm_ggml_tensor * lm_ggml_div_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b,
        bool inplace) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(a, b));

    bool is_node = false;

    if (!inplace && (a->grad || b->grad)) {
        is_node = true;
    }

    if (inplace) {
        LM_GGML_ASSERT(!is_node);
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    result->op   = LM_GGML_OP_DIV;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_div(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b) {
    return lm_ggml_div_impl(ctx, a, b, false);
}

struct lm_ggml_tensor * lm_ggml_div_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b) {
    return lm_ggml_div_impl(ctx, a, b, true);
}

// lm_ggml_sqr

static struct lm_ggml_tensor * lm_ggml_sqr_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        bool inplace) {
    bool is_node = false;

    if (!inplace && (a->grad)) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    result->op   = LM_GGML_OP_SQR;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_sqr(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_sqr_impl(ctx, a, false);
}

struct lm_ggml_tensor * lm_ggml_sqr_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_sqr_impl(ctx, a, true);
}

// lm_ggml_sqrt

static struct lm_ggml_tensor * lm_ggml_sqrt_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        bool inplace) {
    bool is_node = false;

    if (!inplace && (a->grad)) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    result->op   = LM_GGML_OP_SQRT;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_sqrt(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_sqrt_impl(ctx, a, false);
}

struct lm_ggml_tensor * lm_ggml_sqrt_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_sqrt_impl(ctx, a, true);
}

// lm_ggml_log

static struct lm_ggml_tensor * lm_ggml_log_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        bool inplace) {
    bool is_node = false;

    if (!inplace && (a->grad)) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    result->op   = LM_GGML_OP_LOG;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_log(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_log_impl(ctx, a, false);
}

struct lm_ggml_tensor * lm_ggml_log_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_log_impl(ctx, a, true);
}

// lm_ggml_sum

struct lm_ggml_tensor * lm_ggml_sum(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a) {
    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = lm_ggml_new_tensor_1d(ctx, a->type, 1);

    result->op   = LM_GGML_OP_SUM;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_sum_rows

struct lm_ggml_tensor * lm_ggml_sum_rows(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a) {
    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    int64_t ne[4] = {1,1,1,1};
    for (int i=1; i<a->n_dims; ++i) {
        ne[i] = a->ne[i];
    }

    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, a->type, a->n_dims, ne);

    result->op   = LM_GGML_OP_SUM_ROWS;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_mean

struct lm_ggml_tensor * lm_ggml_mean(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a) {
    bool is_node = false;

    if (a->grad) {
        LM_GGML_ASSERT(false); // TODO: implement
        is_node = true;
    }

    int64_t ne[LM_GGML_MAX_DIMS] = { 1, a->ne[1], a->ne[2], a->ne[3] };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, a->n_dims, ne);

    result->op   = LM_GGML_OP_MEAN;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_argmax

struct lm_ggml_tensor * lm_ggml_argmax(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a) {
    LM_GGML_ASSERT(lm_ggml_is_matrix(a));
    bool is_node = false;

    if (a->grad) {
        LM_GGML_ASSERT(false);
        is_node = true;
    }

    int64_t ne[LM_GGML_MAX_DIMS] = { a->ne[1], 1, 1, 1 };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_I32, a->n_dims, ne);

    result->op   = LM_GGML_OP_ARGMAX;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_repeat

struct lm_ggml_tensor * lm_ggml_repeat(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b) {
    LM_GGML_ASSERT(lm_ggml_can_repeat(a, b));

    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, a->type, b->n_dims, b->ne);

    result->op   = LM_GGML_OP_REPEAT;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_repeat_back

struct lm_ggml_tensor * lm_ggml_repeat_back(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b) {
    LM_GGML_ASSERT(lm_ggml_can_repeat(b, a));

    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    if (lm_ggml_are_same_shape(a, b) && !is_node) {
        return a;
    }

    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, a->type, b->n_dims, b->ne);

    result->op   = LM_GGML_OP_REPEAT_BACK;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_concat

struct lm_ggml_tensor * lm_ggml_concat(
    struct lm_ggml_context* ctx,
    struct lm_ggml_tensor* a,
    struct lm_ggml_tensor* b) {
    LM_GGML_ASSERT(a->ne[0] == b->ne[0] && a->ne[1] == b->ne[1] && a->ne[3] == b->ne[3]);

    bool is_node = false;

    if (a->grad || b->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = lm_ggml_new_tensor_4d(ctx, a->type, a->ne[0], a->ne[1], a->ne[2] + b->ne[2], a->ne[3]);

    result->op = LM_GGML_OP_CONCAT;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

// lm_ggml_abs

struct lm_ggml_tensor * lm_ggml_abs(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary(ctx, a, LM_GGML_UNARY_OP_ABS);
}

struct lm_ggml_tensor * lm_ggml_abs_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary_inplace(ctx, a, LM_GGML_UNARY_OP_ABS);
}

// lm_ggml_sgn

struct lm_ggml_tensor * lm_ggml_sgn(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary(ctx, a, LM_GGML_UNARY_OP_SGN);
}

struct lm_ggml_tensor * lm_ggml_sgn_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary_inplace(ctx, a, LM_GGML_UNARY_OP_SGN);
}

// lm_ggml_neg

struct lm_ggml_tensor * lm_ggml_neg(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary(ctx, a, LM_GGML_UNARY_OP_NEG);
}

struct lm_ggml_tensor * lm_ggml_neg_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary_inplace(ctx, a, LM_GGML_UNARY_OP_NEG);
}

// lm_ggml_step

struct lm_ggml_tensor * lm_ggml_step(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary(ctx, a, LM_GGML_UNARY_OP_STEP);
}

struct lm_ggml_tensor * lm_ggml_step_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary_inplace(ctx, a, LM_GGML_UNARY_OP_STEP);
}

// lm_ggml_tanh

struct lm_ggml_tensor * lm_ggml_tanh(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary(ctx, a, LM_GGML_UNARY_OP_TANH);
}

struct lm_ggml_tensor * lm_ggml_tanh_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary_inplace(ctx, a, LM_GGML_UNARY_OP_TANH);
}

// lm_ggml_elu

struct lm_ggml_tensor * lm_ggml_elu(
    struct lm_ggml_context * ctx,
    struct lm_ggml_tensor  * a) {
    return lm_ggml_unary(ctx, a, LM_GGML_UNARY_OP_ELU);
}

struct lm_ggml_tensor * lm_ggml_elu_inplace(
    struct lm_ggml_context * ctx,
    struct lm_ggml_tensor  * a) {
    return lm_ggml_unary_inplace(ctx, a, LM_GGML_UNARY_OP_ELU);
}

// lm_ggml_relu

struct lm_ggml_tensor * lm_ggml_relu(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary(ctx, a, LM_GGML_UNARY_OP_RELU);
}

struct lm_ggml_tensor * lm_ggml_relu_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary_inplace(ctx, a, LM_GGML_UNARY_OP_RELU);
}

// lm_ggml_gelu

struct lm_ggml_tensor * lm_ggml_gelu(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary(ctx, a, LM_GGML_UNARY_OP_GELU);
}

struct lm_ggml_tensor * lm_ggml_gelu_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary_inplace(ctx, a, LM_GGML_UNARY_OP_GELU);
}

// lm_ggml_gelu_quick

struct lm_ggml_tensor * lm_ggml_gelu_quick(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary(ctx, a, LM_GGML_UNARY_OP_GELU_QUICK);
}

struct lm_ggml_tensor * lm_ggml_gelu_quick_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary_inplace(ctx, a, LM_GGML_UNARY_OP_GELU_QUICK);
}

// lm_ggml_silu

struct lm_ggml_tensor * lm_ggml_silu(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary(ctx, a, LM_GGML_UNARY_OP_SILU);
}

struct lm_ggml_tensor * lm_ggml_silu_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_unary_inplace(ctx, a, LM_GGML_UNARY_OP_SILU);
}

// lm_ggml_silu_back

struct lm_ggml_tensor * lm_ggml_silu_back(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b) {
    bool is_node = false;

    if (a->grad || b->grad) {
        // TODO: implement backward
        is_node = true;
    }

    struct lm_ggml_tensor * result = lm_ggml_dup_tensor(ctx, a);

    result->op   = LM_GGML_OP_SILU_BACK;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

// lm_ggml_norm

static struct lm_ggml_tensor * lm_ggml_norm_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        float eps,
        bool inplace) {
    bool is_node = false;

    if (!inplace && (a->grad)) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    lm_ggml_set_op_params(result, &eps, sizeof(eps));

    result->op   = LM_GGML_OP_NORM;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_norm(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        float eps) {
    return lm_ggml_norm_impl(ctx, a, eps, false);
}

struct lm_ggml_tensor * lm_ggml_norm_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        float eps) {
    return lm_ggml_norm_impl(ctx, a, eps, true);
}

// lm_ggml_rms_norm

static struct lm_ggml_tensor * lm_ggml_rms_norm_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        float eps,
        bool inplace) {
    bool is_node = false;

    if (!inplace && (a->grad)) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    lm_ggml_set_op_params(result, &eps, sizeof(eps));

    result->op   = LM_GGML_OP_RMS_NORM;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_rms_norm(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        float  eps) {
    return lm_ggml_rms_norm_impl(ctx, a, eps, false);
}

struct lm_ggml_tensor * lm_ggml_rms_norm_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        float eps) {
    return lm_ggml_rms_norm_impl(ctx, a, eps, true);
}

// lm_ggml_rms_norm_back

struct lm_ggml_tensor * lm_ggml_rms_norm_back(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        float  eps) {
    bool is_node = false;

    if (a->grad) {
        // TODO: implement backward
        is_node = true;
    }

    struct lm_ggml_tensor * result = lm_ggml_dup_tensor(ctx, a);

    lm_ggml_set_op_params(result, &eps, sizeof(eps));

    result->op   = LM_GGML_OP_RMS_NORM_BACK;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

// lm_ggml_group_norm

static struct lm_ggml_tensor * lm_ggml_group_norm_impl(
    struct lm_ggml_context * ctx,
    struct lm_ggml_tensor * a,
    int n_groups,
    bool inplace) {

    bool is_node = false;
    if (!inplace && (a->grad)) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    result->op = LM_GGML_OP_GROUP_NORM;
    result->op_params[0] = n_groups;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = NULL; // TODO: maybe store epsilon here?

    return result;
}

struct lm_ggml_tensor * lm_ggml_group_norm(
    struct lm_ggml_context * ctx,
    struct lm_ggml_tensor * a,
    int n_groups) {
    return lm_ggml_group_norm_impl(ctx, a, n_groups, false);
}

struct lm_ggml_tensor * lm_ggml_group_norm_inplace(
    struct lm_ggml_context * ctx,
    struct lm_ggml_tensor * a,
    int n_groups) {
    return lm_ggml_group_norm_impl(ctx, a, n_groups, true);
}

// lm_ggml_mul_mat

struct lm_ggml_tensor * lm_ggml_mul_mat(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b) {
    LM_GGML_ASSERT(lm_ggml_can_mul_mat(a, b));
    LM_GGML_ASSERT(!lm_ggml_is_transposed(a));

    bool is_node = false;

    if (a->grad || b->grad) {
        is_node = true;
    }

    const int64_t ne[4] = { a->ne[1], b->ne[1], b->ne[2], b->ne[3] };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, MAX(a->n_dims, b->n_dims), ne);

    result->op   = LM_GGML_OP_MUL_MAT;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

// lm_ggml_out_prod

struct lm_ggml_tensor * lm_ggml_out_prod(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b) {
    LM_GGML_ASSERT(lm_ggml_can_out_prod(a, b));
    LM_GGML_ASSERT(!lm_ggml_is_transposed(a));

    bool is_node = false;

    if (a->grad || b->grad) {
        is_node = true;
    }

    // a is broadcastable to b for ne[2] and ne[3] -> use b->ne[2] and b->ne[3]
    const int64_t ne[4] = { a->ne[0], b->ne[0], b->ne[2], b->ne[3] };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, MAX(a->n_dims, b->n_dims), ne);

    result->op   = LM_GGML_OP_OUT_PROD;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

// lm_ggml_scale

static struct lm_ggml_tensor * lm_ggml_scale_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        bool inplace) {
    LM_GGML_ASSERT(lm_ggml_is_scalar(b));
    LM_GGML_ASSERT(lm_ggml_is_padded_1d(a));

    bool is_node = false;

    if (a->grad || b->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    result->op   = LM_GGML_OP_SCALE;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_scale(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b) {
    return lm_ggml_scale_impl(ctx, a, b, false);
}

struct lm_ggml_tensor * lm_ggml_scale_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b) {
    return lm_ggml_scale_impl(ctx, a, b, true);
}

// lm_ggml_set

static struct lm_ggml_tensor * lm_ggml_set_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        size_t                nb1,
        size_t                nb2,
        size_t                nb3,
        size_t                offset,
        bool inplace) {
    LM_GGML_ASSERT(lm_ggml_nelements(a) >= lm_ggml_nelements(b));

    bool is_node = false;

    if (a->grad || b->grad) {
        is_node = true;
    }

    // make a view of the destination
    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    int32_t params[] = { nb1, nb2, nb3, offset, inplace ? 1 : 0 };
    lm_ggml_set_op_params(result, params, sizeof(params));

    result->op   = LM_GGML_OP_SET;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_set(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor *  a,
        struct lm_ggml_tensor *  b,
        size_t                nb1,
        size_t                nb2,
        size_t                nb3,
        size_t                offset) {
    return lm_ggml_set_impl(ctx, a, b, nb1, nb2, nb3, offset, false);
}

struct lm_ggml_tensor * lm_ggml_set_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor *  a,
        struct lm_ggml_tensor *  b,
        size_t                nb1,
        size_t                nb2,
        size_t                nb3,
        size_t                offset) {
    return lm_ggml_set_impl(ctx, a, b, nb1, nb2, nb3, offset, true);
}

struct lm_ggml_tensor * lm_ggml_set_1d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor *  a,
        struct lm_ggml_tensor *  b,
        size_t                offset) {
    return lm_ggml_set_impl(ctx, a, b, a->nb[1], a->nb[2], a->nb[3], offset, false);
}

struct lm_ggml_tensor * lm_ggml_set_1d_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor *  a,
        struct lm_ggml_tensor *  b,
        size_t                offset) {
    return lm_ggml_set_impl(ctx, a, b, a->nb[1], a->nb[2], a->nb[3], offset, true);
}

struct lm_ggml_tensor * lm_ggml_set_2d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor *  a,
        struct lm_ggml_tensor *  b,
        size_t                nb1,
        size_t                offset) {
    return lm_ggml_set_impl(ctx, a, b, nb1, a->nb[2], a->nb[3], offset, false);
}

struct lm_ggml_tensor * lm_ggml_set_2d_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor *  a,
        struct lm_ggml_tensor *  b,
        size_t                nb1,
        size_t                offset) {
    return lm_ggml_set_impl(ctx, a, b, nb1, a->nb[2], a->nb[3], offset, false);
}

// lm_ggml_cpy

static struct lm_ggml_tensor * lm_ggml_cpy_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        bool inplace) {
    LM_GGML_ASSERT(lm_ggml_nelements(a) == lm_ggml_nelements(b));

    bool is_node = false;

    if (!inplace && (a->grad || b->grad)) {
        is_node = true;
    }

    // make a view of the destination
    struct lm_ggml_tensor * result = lm_ggml_view_tensor(ctx, b);
    if (strlen(b->name) > 0) {
        lm_ggml_format_name(result, "%s (copy of %s)", b->name, a->name);
    } else {
        lm_ggml_format_name(result, "%s (copy)", a->name);
    }

    result->op   = LM_GGML_OP_CPY;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_cpy(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b) {
    return lm_ggml_cpy_impl(ctx, a, b, false);
}

struct lm_ggml_tensor * lm_ggml_cpy_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b) {
    return lm_ggml_cpy_impl(ctx, a, b, true);
}

// lm_ggml_cont

static struct lm_ggml_tensor * lm_ggml_cont_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        bool inplace) {
    bool is_node = false;

    if (!inplace && a->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);
    lm_ggml_format_name(result, "%s (cont)", a->name);

    result->op   = LM_GGML_OP_CONT;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_cont(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a) {
    return lm_ggml_cont_impl(ctx, a, false);
}

struct lm_ggml_tensor * lm_ggml_cont_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a) {
    return lm_ggml_cont_impl(ctx, a, true);
}

// make contiguous, with new shape
LM_GGML_API struct lm_ggml_tensor * lm_ggml_cont_1d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int64_t               ne0) {
    return lm_ggml_cont_4d(ctx, a, ne0, 1, 1, 1);
}

LM_GGML_API struct lm_ggml_tensor * lm_ggml_cont_2d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int64_t               ne0,
        int64_t               ne1) {
    return lm_ggml_cont_4d(ctx, a, ne0, ne1, 1, 1);
}

LM_GGML_API struct lm_ggml_tensor * lm_ggml_cont_3d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int64_t               ne0,
        int64_t               ne1,
        int64_t               ne2) {
    return lm_ggml_cont_4d(ctx, a, ne0, ne1, ne2, 1);
}

struct lm_ggml_tensor * lm_ggml_cont_4d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int64_t               ne0,
        int64_t               ne1,
        int64_t               ne2,
        int64_t               ne3) {
    LM_GGML_ASSERT(lm_ggml_nelements(a) == (ne0*ne1*ne2*ne3));

    bool is_node = false;

    struct lm_ggml_tensor * result = lm_ggml_new_tensor_4d(ctx, a->type, ne0, ne1, ne2, ne3);
    lm_ggml_format_name(result, "%s (cont)", a->name);

    result->op   = LM_GGML_OP_CONT;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_reshape

struct lm_ggml_tensor * lm_ggml_reshape(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * b) {
    LM_GGML_ASSERT(lm_ggml_is_contiguous(a));
    // as only the shape of b is relevant, and not its memory layout, b is allowed to be non contiguous.
    LM_GGML_ASSERT(lm_ggml_nelements(a) == lm_ggml_nelements(b));

    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    if (b->grad) {
        // gradient propagation is not supported
        //LM_GGML_ASSERT(false);
    }

    struct lm_ggml_tensor * result = lm_ggml_new_tensor_impl(ctx, a->type, b->n_dims, b->ne, a, 0);
    lm_ggml_format_name(result, "%s (reshaped)", a->name);

    result->op   = LM_GGML_OP_RESHAPE;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_reshape_1d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int64_t               ne0) {
    LM_GGML_ASSERT(lm_ggml_is_contiguous(a));
    LM_GGML_ASSERT(lm_ggml_nelements(a) == ne0);

    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    const int64_t ne[1] = { ne0 };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor_impl(ctx, a->type, 1, ne, a, 0);
    lm_ggml_format_name(result, "%s (reshaped)", a->name);

    result->op   = LM_GGML_OP_RESHAPE;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_reshape_2d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int64_t               ne0,
        int64_t               ne1) {
    LM_GGML_ASSERT(lm_ggml_is_contiguous(a));
    LM_GGML_ASSERT(lm_ggml_nelements(a) == ne0*ne1);

    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    const int64_t ne[2] = { ne0, ne1 };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor_impl(ctx, a->type, 2, ne, a, 0);
    lm_ggml_format_name(result, "%s (reshaped)", a->name);

    result->op   = LM_GGML_OP_RESHAPE;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_reshape_3d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int64_t               ne0,
        int64_t               ne1,
        int64_t               ne2) {
    LM_GGML_ASSERT(lm_ggml_is_contiguous(a));
    LM_GGML_ASSERT(lm_ggml_nelements(a) == ne0*ne1*ne2);

    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    const int64_t ne[3] = { ne0, ne1, ne2 };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor_impl(ctx, a->type, 3, ne, a, 0);
    lm_ggml_format_name(result, "%s (reshaped)", a->name);

    result->op   = LM_GGML_OP_RESHAPE;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_reshape_4d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int64_t               ne0,
        int64_t               ne1,
        int64_t               ne2,
        int64_t               ne3) {
    LM_GGML_ASSERT(lm_ggml_is_contiguous(a));
    LM_GGML_ASSERT(lm_ggml_nelements(a) == ne0*ne1*ne2*ne3);

    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    const int64_t ne[4] = { ne0, ne1, ne2, ne3 };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor_impl(ctx, a->type, 4, ne, a, 0);
    lm_ggml_format_name(result, "%s (reshaped)", a->name);

    result->op   = LM_GGML_OP_RESHAPE;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

static struct lm_ggml_tensor * lm_ggml_view_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int                   n_dims,
        const int64_t       * ne,
        size_t                offset) {

    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = lm_ggml_new_tensor_impl(ctx, a->type, n_dims, ne, a, offset);
    lm_ggml_format_name(result, "%s (view)", a->name);

    lm_ggml_set_op_params(result, &offset, sizeof(offset));

    result->op   = LM_GGML_OP_VIEW;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_view_1d

struct lm_ggml_tensor * lm_ggml_view_1d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int64_t               ne0,
        size_t                offset) {

    struct lm_ggml_tensor * result = lm_ggml_view_impl(ctx, a, 1, &ne0, offset);

    return result;
}

// lm_ggml_view_2d

struct lm_ggml_tensor * lm_ggml_view_2d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int64_t               ne0,
        int64_t               ne1,
        size_t                nb1,
        size_t                offset) {

    const int64_t ne[2] = { ne0, ne1 };

    struct lm_ggml_tensor * result = lm_ggml_view_impl(ctx, a, 2, ne, offset);

    result->nb[1] = nb1;
    result->nb[2] = result->nb[1]*ne1;
    result->nb[3] = result->nb[2];

    return result;
}

// lm_ggml_view_3d

struct lm_ggml_tensor * lm_ggml_view_3d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int64_t               ne0,
        int64_t               ne1,
        int64_t               ne2,
        size_t                nb1,
        size_t                nb2,
        size_t                offset) {

    const int64_t ne[3] = { ne0, ne1, ne2 };

    struct lm_ggml_tensor * result = lm_ggml_view_impl(ctx, a, 3, ne, offset);

    result->nb[1] = nb1;
    result->nb[2] = nb2;
    result->nb[3] = result->nb[2]*ne2;

    return result;
}

// lm_ggml_view_4d

struct lm_ggml_tensor * lm_ggml_view_4d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int64_t               ne0,
        int64_t               ne1,
        int64_t               ne2,
        int64_t               ne3,
        size_t                nb1,
        size_t                nb2,
        size_t                nb3,
        size_t                offset) {

    const int64_t ne[4] = { ne0, ne1, ne2, ne3 };

    struct lm_ggml_tensor * result = lm_ggml_view_impl(ctx, a, 4, ne, offset);

    result->nb[1] = nb1;
    result->nb[2] = nb2;
    result->nb[3] = nb3;

    return result;
}

// lm_ggml_permute

struct lm_ggml_tensor * lm_ggml_permute(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int                   axis0,
        int                   axis1,
        int                   axis2,
        int                   axis3) {
    LM_GGML_ASSERT(axis0 >= 0 && axis0 < LM_GGML_MAX_DIMS);
    LM_GGML_ASSERT(axis1 >= 0 && axis1 < LM_GGML_MAX_DIMS);
    LM_GGML_ASSERT(axis2 >= 0 && axis2 < LM_GGML_MAX_DIMS);
    LM_GGML_ASSERT(axis3 >= 0 && axis3 < LM_GGML_MAX_DIMS);

    LM_GGML_ASSERT(axis0 != axis1);
    LM_GGML_ASSERT(axis0 != axis2);
    LM_GGML_ASSERT(axis0 != axis3);
    LM_GGML_ASSERT(axis1 != axis2);
    LM_GGML_ASSERT(axis1 != axis3);
    LM_GGML_ASSERT(axis2 != axis3);

    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = lm_ggml_view_tensor(ctx, a);
    lm_ggml_format_name(result, "%s (permuted)", a->name);

    int ne[LM_GGML_MAX_DIMS];
    int nb[LM_GGML_MAX_DIMS];

    ne[axis0] = a->ne[0];
    ne[axis1] = a->ne[1];
    ne[axis2] = a->ne[2];
    ne[axis3] = a->ne[3];

    nb[axis0] = a->nb[0];
    nb[axis1] = a->nb[1];
    nb[axis2] = a->nb[2];
    nb[axis3] = a->nb[3];

    result->ne[0] = ne[0];
    result->ne[1] = ne[1];
    result->ne[2] = ne[2];
    result->ne[3] = ne[3];

    result->nb[0] = nb[0];
    result->nb[1] = nb[1];
    result->nb[2] = nb[2];
    result->nb[3] = nb[3];

    result->op   = LM_GGML_OP_PERMUTE;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    int32_t params[] = { axis0, axis1, axis2, axis3 };
    lm_ggml_set_op_params(result, params, sizeof(params));

    return result;
}

// lm_ggml_transpose

struct lm_ggml_tensor * lm_ggml_transpose(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = lm_ggml_view_tensor(ctx, a);
    lm_ggml_format_name(result, "%s (transposed)", a->name);

    result->ne[0] = a->ne[1];
    result->ne[1] = a->ne[0];

    result->nb[0] = a->nb[1];
    result->nb[1] = a->nb[0];

    result->op   = LM_GGML_OP_TRANSPOSE;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_get_rows

struct lm_ggml_tensor * lm_ggml_get_rows(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b) {
    LM_GGML_ASSERT(lm_ggml_is_matrix(a) && lm_ggml_is_vector(b) && b->type == LM_GGML_TYPE_I32);

    bool is_node = false;

    if (a->grad || b->grad) {
        is_node = true;
    }

    // TODO: implement non F32 return
    //struct lm_ggml_tensor * result = lm_ggml_new_tensor_2d(ctx, a->type, a->ne[0], b->ne[0]);
    struct lm_ggml_tensor * result = lm_ggml_new_tensor_2d(ctx, LM_GGML_TYPE_F32, a->ne[0], b->ne[0]);

    result->op   = LM_GGML_OP_GET_ROWS;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

// lm_ggml_get_rows_back

struct lm_ggml_tensor * lm_ggml_get_rows_back(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        struct lm_ggml_tensor  * c) {
    LM_GGML_ASSERT(lm_ggml_is_matrix(a) && lm_ggml_is_vector(b) && b->type == LM_GGML_TYPE_I32);
    LM_GGML_ASSERT(lm_ggml_is_matrix(c) && (a->ne[0] == c->ne[0]));

    bool is_node = false;

    if (a->grad || b->grad) {
        is_node = true;
    }

    // TODO: implement non F32 return
    //struct lm_ggml_tensor * result = lm_ggml_new_tensor_2d(ctx, a->type, a->ne[0], b->ne[0]);
    struct lm_ggml_tensor * result = lm_ggml_new_tensor_2d(ctx, LM_GGML_TYPE_F32, c->ne[0], c->ne[1]);

    result->op   = LM_GGML_OP_GET_ROWS_BACK;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

// lm_ggml_diag

struct lm_ggml_tensor * lm_ggml_diag(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    LM_GGML_ASSERT(a->ne[1] == 1);
    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    const int64_t ne[4] = { a->ne[0], a->ne[0], a->ne[2], a->ne[3] };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, a->type, MAX(a->n_dims, 2), ne);

    result->op   = LM_GGML_OP_DIAG;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_diag_mask_inf

static struct lm_ggml_tensor * lm_ggml_diag_mask_inf_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int                   n_past,
        bool                  inplace) {
    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    int32_t params[] = { n_past };
    lm_ggml_set_op_params(result, params, sizeof(params));

    result->op   = LM_GGML_OP_DIAG_MASK_INF;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_diag_mask_inf(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int                   n_past) {
    return lm_ggml_diag_mask_inf_impl(ctx, a, n_past, false);
}

struct lm_ggml_tensor * lm_ggml_diag_mask_inf_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int                   n_past) {
    return lm_ggml_diag_mask_inf_impl(ctx, a, n_past, true);
}

// lm_ggml_diag_mask_zero

static struct lm_ggml_tensor * lm_ggml_diag_mask_zero_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int                   n_past,
        bool                  inplace) {
    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    int32_t params[] = { n_past };
    lm_ggml_set_op_params(result, params, sizeof(params));

    result->op   = LM_GGML_OP_DIAG_MASK_ZERO;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_diag_mask_zero(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int                   n_past) {
    return lm_ggml_diag_mask_zero_impl(ctx, a, n_past, false);
}

struct lm_ggml_tensor * lm_ggml_diag_mask_zero_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int                   n_past) {
    return lm_ggml_diag_mask_zero_impl(ctx, a, n_past, true);
}

// lm_ggml_soft_max

static struct lm_ggml_tensor * lm_ggml_soft_max_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        bool                  inplace) {
    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    result->op   = LM_GGML_OP_SOFT_MAX;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_soft_max(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_soft_max_impl(ctx, a, false);
}

struct lm_ggml_tensor * lm_ggml_soft_max_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a) {
    return lm_ggml_soft_max_impl(ctx, a, true);
}

// lm_ggml_soft_max_back

static struct lm_ggml_tensor * lm_ggml_soft_max_back_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        bool                  inplace) {
    bool is_node = false;

    if (a->grad || b->grad) {
        is_node = true; // TODO : implement backward pass
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    result->op   = LM_GGML_OP_SOFT_MAX_BACK;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_soft_max_back(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b) {
    return lm_ggml_soft_max_back_impl(ctx, a, b, false);
}

struct lm_ggml_tensor * lm_ggml_soft_max_back_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b) {
    return lm_ggml_soft_max_back_impl(ctx, a, b, true);
}

// lm_ggml_rope

static struct lm_ggml_tensor * lm_ggml_rope_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        int                   n_dims,
        int                   mode,
        int                   n_ctx,
        float                 freq_base,
        float                 freq_scale,
        float                 xpos_base,
        bool                  xpos_down,
        bool                  inplace) {
    LM_GGML_ASSERT(lm_ggml_is_vector(b));
    LM_GGML_ASSERT(b->type == LM_GGML_TYPE_I32);
    LM_GGML_ASSERT(a->ne[2] == b->ne[0]);

    bool is_node = false;

    if (a->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    int32_t params[8] = { /*n_past*/ 0, n_dims, mode, n_ctx };
    memcpy(params + 4, &freq_base,  sizeof(float));
    memcpy(params + 5, &freq_scale, sizeof(float));
    memcpy(params + 6, &xpos_base,  sizeof(float));
    memcpy(params + 7, &xpos_down,  sizeof(bool));
    lm_ggml_set_op_params(result, params, sizeof(params));

    result->op   = LM_GGML_OP_ROPE;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_rope(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        int                   n_dims,
        int                   mode,
        int                   n_ctx) {
    return lm_ggml_rope_impl(ctx, a, b, n_dims, mode, n_ctx, 10000.0f, 1.0f, 0.0f, false, false);
}

struct lm_ggml_tensor * lm_ggml_rope_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        int                   n_dims,
        int                   mode,
        int                   n_ctx) {
    return lm_ggml_rope_impl(ctx, a, b, n_dims, mode, n_ctx, 10000.0f, 1.0f, 0.0f, false, true);
}

struct lm_ggml_tensor * lm_ggml_rope_custom(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        int                   n_dims,
        int                   mode,
        int                   n_ctx,
        float                 freq_base,
        float                 freq_scale) {
    return lm_ggml_rope_impl(ctx, a, b, n_dims, mode, n_ctx, freq_base, freq_scale, 0.0f, false, false);
}

struct lm_ggml_tensor * lm_ggml_rope_custom_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        int                   n_dims,
        int                   mode,
        int                   n_ctx,
        float                 freq_base,
        float                 freq_scale) {
    return lm_ggml_rope_impl(ctx, a, b, n_dims, mode, n_ctx, freq_base, freq_scale, 0.0f, false, true);
}

struct lm_ggml_tensor * lm_ggml_rope_xpos_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        int                   n_dims,
        float                 base,
        bool                  down) {
    return lm_ggml_rope_impl(ctx, a, b, n_dims, 0, 0, 10000.0f, 1.0f, base, down, true);
}

// lm_ggml_rope_back

struct lm_ggml_tensor * lm_ggml_rope_back(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        int                   n_dims,
        int                   mode,
        int                   n_ctx,
        float                 freq_base,
        float                 freq_scale,
        float                 xpos_base,
        bool                  xpos_down) {
    LM_GGML_ASSERT(lm_ggml_is_vector(b));
    LM_GGML_ASSERT(b->type == LM_GGML_TYPE_I32);
    LM_GGML_ASSERT(a->ne[2] == b->ne[0]);

    LM_GGML_ASSERT((mode & 4) == 0 && "lm_ggml_rope_back() for ChatGLM not implemented yet");

    bool is_node = false;

    if (a->grad) {
        is_node = false; // TODO: implement backward
    }

    struct lm_ggml_tensor * result = lm_ggml_dup_tensor(ctx, a);

    int32_t params[8] = { /*n_past*/ 0, n_dims, mode, n_ctx };
    memcpy(params + 4, &freq_base,  sizeof(float));
    memcpy(params + 5, &freq_scale, sizeof(float));
    memcpy(params + 6, &xpos_base,  sizeof(float));
    memcpy(params + 7, &xpos_down,  sizeof(bool));
    lm_ggml_set_op_params(result, params, sizeof(params));

    result->op   = LM_GGML_OP_ROPE_BACK;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

// lm_ggml_alibi

struct lm_ggml_tensor * lm_ggml_alibi(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int                   n_past,
        int                   n_head,
        float                 bias_max) {
    LM_GGML_ASSERT(n_past >= 0);
    bool is_node = false;

    if (a->grad) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    // TODO: when implement backward, fix this:
    //struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);
    struct lm_ggml_tensor * result = lm_ggml_view_tensor(ctx, a);

    int32_t op_params[3] = { n_past, n_head };
    memcpy(op_params + 2, &bias_max, sizeof(float));
    lm_ggml_set_op_params(result, op_params, sizeof(op_params));

    result->op   = LM_GGML_OP_ALIBI;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_clamp

struct lm_ggml_tensor * lm_ggml_clamp(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        float                 min,
        float                 max) {
    bool is_node = false;

    if (a->grad) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    // TODO: when implement backward, fix this:
    struct lm_ggml_tensor * result = lm_ggml_view_tensor(ctx, a);

    float params[] = { min, max };
    lm_ggml_set_op_params(result, params, sizeof(params));

    result->op   = LM_GGML_OP_CLAMP;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_conv_1d

static int64_t lm_ggml_calc_conv_output_size(int64_t ins, int64_t ks, int s, int p, int d) {
    return (ins + 2 * p - d * (ks - 1) - 1) / s + 1;
}

// im2col: [N, IC, IL] => [N, OL, IC*K]
// a: [OC，IC, K]
// b: [N, IC, IL]
// result: [N, OL, IC*K]
static struct lm_ggml_tensor * lm_ggml_conv_1d_stage_0(
    struct lm_ggml_context * ctx,
    struct lm_ggml_tensor  * a,
    struct lm_ggml_tensor  * b,
    int                   s0,
    int                   p0,
    int                   d0) {
    LM_GGML_ASSERT(a->ne[1] == b->ne[1]);
    bool is_node = false;

    if (a->grad || b->grad) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    const int64_t OL = lm_ggml_calc_conv_output_size(b->ne[0], a->ne[0], s0, p0, d0);

    const int64_t ne[4] = {
        a->ne[1] * a->ne[0],
        OL,
        b->ne[2],
        1,
    };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F16, 4, ne);

    int32_t params[] = { s0, p0, d0 };
    lm_ggml_set_op_params(result, params, sizeof(params));

    result->op = LM_GGML_OP_CONV_1D_STAGE_0;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

// lm_ggml_conv_1d_stage_1

// gemm: [N, OC, OL] = [OC, IC * K] x [N*OL, IC * K]
// a: [OC, IC, K]
// b: [N, OL, IC * K]
// result: [N, OC, OL]
static struct lm_ggml_tensor * lm_ggml_conv_1d_stage_1(
    struct lm_ggml_context * ctx,
    struct lm_ggml_tensor  * a,
    struct lm_ggml_tensor  * b) {

    bool is_node = false;

    if (a->grad || b->grad) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    const int64_t ne[4] = {
        b->ne[1],
        a->ne[2],
        b->ne[2],
        1,
    };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, 4, ne);

    result->op = LM_GGML_OP_CONV_1D_STAGE_1;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

// lm_ggml_conv_1d

LM_GGML_API struct lm_ggml_tensor * lm_ggml_conv_1d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        int                   s0,
        int                   p0,
        int                   d0) {
    struct lm_ggml_tensor * result = lm_ggml_conv_1d_stage_0(ctx, a, b, s0, p0, d0);
    result = lm_ggml_conv_1d_stage_1(ctx, a, result);
    return result;
}

// LM_GGML_API struct lm_ggml_tensor * lm_ggml_conv_1d(
//         struct lm_ggml_context * ctx,
//         struct lm_ggml_tensor  * a,
//         struct lm_ggml_tensor  * b,
//         int                   s0,
//         int                   p0,
//         int                   d0) {
//     LM_GGML_ASSERT(lm_ggml_is_matrix(b));
//     LM_GGML_ASSERT(a->ne[1] == b->ne[1]);
//     bool is_node = false;

//     if (a->grad || b->grad) {
//         LM_GGML_ASSERT(false); // TODO: implement backward
//         is_node = true;
//     }

//     const int64_t ne[4] = {
//         lm_ggml_calc_conv_output_size(b->ne[0], a->ne[0], s0, p0, d0),
//         a->ne[2], 1, 1,
//     };
//     struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, 2, ne);

//     int32_t params[] = { s0, p0, d0 };
//     lm_ggml_set_op_params(result, params, sizeof(params));

//     result->op = LM_GGML_OP_CONV_1D;
//     result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
//     result->src[0] = a;
//     result->src[1] = b;

//     return result;
// }

// lm_ggml_conv_1d_ph

struct lm_ggml_tensor* lm_ggml_conv_1d_ph(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        int                   s,
        int                   d) {
    return lm_ggml_conv_1d(ctx, a, b, s, a->ne[0] / 2, d);
}

// lm_ggml_conv_transpose_1d

static int64_t lm_ggml_calc_conv_transpose_1d_output_size(int64_t ins, int64_t ks, int s, int p, int d) {
    return (ins - 1) * s - 2 * p + d * (ks - 1) + 1;
}

LM_GGML_API struct lm_ggml_tensor * lm_ggml_conv_transpose_1d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        int                   s0,
        int                   p0,
        int                   d0) {
    LM_GGML_ASSERT(lm_ggml_is_matrix(b));
    LM_GGML_ASSERT(a->ne[2] == b->ne[1]);
    LM_GGML_ASSERT(a->ne[3] == 1);

    LM_GGML_ASSERT(p0 == 0);
    LM_GGML_ASSERT(d0 == 1);

    bool is_node = false;

    if (a->grad || b->grad) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    const int64_t ne[4] = {
        lm_ggml_calc_conv_transpose_1d_output_size(b->ne[0], a->ne[0], s0, 0 /*p0*/, 1 /*d0*/),
        a->ne[1], b->ne[2], 1,
    };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, 4, ne);

    int32_t params[] = { s0, p0, d0 };
    lm_ggml_set_op_params(result, params, sizeof(params));

    result->op = LM_GGML_OP_CONV_TRANSPOSE_1D;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

// lm_ggml_conv_2d

// im2col: [N, IC, IH, IW] => [N, OH, OW, IC*KH*KW]
// a: [OC，IC, KH, KW]
// b: [N, IC, IH, IW]
// result: [N, OH, OW, IC*KH*KW]
static struct lm_ggml_tensor * lm_ggml_conv_2d_stage_0(
    struct lm_ggml_context * ctx,
    struct lm_ggml_tensor  * a,
    struct lm_ggml_tensor  * b,
    int                  s0,
    int                  s1,
    int                  p0,
    int                  p1,
    int                  d0,
    int                  d1) {

    LM_GGML_ASSERT(a->ne[2] == b->ne[2]);
    bool is_node = false;

    if (a->grad || b->grad) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    const int64_t OH = lm_ggml_calc_conv_output_size(b->ne[1], a->ne[1], s1, p1, d1);
    const int64_t OW = lm_ggml_calc_conv_output_size(b->ne[0], a->ne[0], s0, p0, d0);

    const int64_t ne[4] = {
        a->ne[2] * a->ne[1] * a->ne[0],
        OW,
        OH,
        b->ne[3],
    };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F16, 4, ne);

    int32_t params[] = { s0, s1, p0, p1, d0, d1 };
    lm_ggml_set_op_params(result, params, sizeof(params));

    result->op = LM_GGML_OP_CONV_2D_STAGE_0;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;

}

// gemm: [N, OC, OH, OW] = [OC, IC * KH * KW] x [N*OH*OW, IC * KH * KW]
// a: [OC, IC, KH, KW]
// b: [N, OH, OW, IC * KH * KW]
// result: [N, OC, OH, OW]
static struct lm_ggml_tensor * lm_ggml_conv_2d_stage_1(
    struct lm_ggml_context * ctx,
    struct lm_ggml_tensor  * a,
    struct lm_ggml_tensor  * b) {

    bool is_node = false;

    if (a->grad || b->grad) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    const int64_t ne[4] = {
        b->ne[1],
        b->ne[2],
        a->ne[3],
        b->ne[3],
    };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, 4, ne);

    result->op = LM_GGML_OP_CONV_2D_STAGE_1;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;

}

// a: [OC，IC, KH, KW]
// b: [N, IC, IH, IW]
// result: [N, OC, OH, OW]
struct lm_ggml_tensor * lm_ggml_conv_2d(
    struct lm_ggml_context * ctx,
    struct lm_ggml_tensor  * a,
    struct lm_ggml_tensor  * b,
    int                  s0,
    int                  s1,
    int                  p0,
    int                  p1,
    int                  d0,
    int                  d1) {

    struct lm_ggml_tensor * result = lm_ggml_conv_2d_stage_0(ctx, a, b, s0, s1, p0, p1, d0, d1); // [N, OH, OW, IC * KH * KW]
    result = lm_ggml_conv_2d_stage_1(ctx, a, result);

    return result;

}

// lm_ggml_conv_2d_sk_p0
struct lm_ggml_tensor * lm_ggml_conv_2d_sk_p0(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b) {
    return lm_ggml_conv_2d(ctx, a, b, a->ne[0], a->ne[1], 0, 0, 1, 1);
}

// lm_ggml_conv_2d_s1_ph

struct lm_ggml_tensor * lm_ggml_conv_2d_s1_ph(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b) {
    return lm_ggml_conv_2d(ctx, a, b, 1, 1, a->ne[0] / 2, a->ne[1] / 2, 1, 1);
}

// lm_ggml_conv_transpose_2d_p0

static int64_t lm_ggml_calc_conv_transpose_output_size(int64_t ins, int64_t ks, int s, int p) {
    return (ins - 1) * s - 2 * p + ks;
}

struct lm_ggml_tensor * lm_ggml_conv_transpose_2d_p0(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b,
        int                   stride) {
    LM_GGML_ASSERT(a->ne[3] == b->ne[2]);

    bool is_node = false;

    if (a->grad || b->grad) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    const int64_t ne[4] = {
        lm_ggml_calc_conv_transpose_output_size(b->ne[0], a->ne[0], stride, 0 /*p0*/),
        lm_ggml_calc_conv_transpose_output_size(b->ne[1], a->ne[1], stride, 0 /*p1*/),
        a->ne[2], b->ne[3],
    };

    struct lm_ggml_tensor* result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, 4, ne);

    lm_ggml_set_op_params_i32(result, 0, stride);

    result->op = LM_GGML_OP_CONV_TRANSPOSE_2D;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

// lm_ggml_pool_*

static int64_t lm_ggml_calc_pool_output_size(int64_t ins, int ks, int s, int p) {
    return (ins + 2 * p - ks) / s + 1;
}

// lm_ggml_pool_1d

struct lm_ggml_tensor * lm_ggml_pool_1d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        enum lm_ggml_op_pool     op,
        int                   k0,
        int                   s0,
        int                   p0) {

    bool is_node = false;

    if (a->grad) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    const int64_t ne[3] = {
        lm_ggml_calc_pool_output_size(a->ne[0], k0, s0, p0),
        a->ne[1],
    };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, 2, ne);

    int32_t params[] = { op, k0, s0, p0 };
    lm_ggml_set_op_params(result, params, sizeof(params));

    result->op = LM_GGML_OP_POOL_1D;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_pool_2d

struct lm_ggml_tensor * lm_ggml_pool_2d(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        enum lm_ggml_op_pool     op,
        int                   k0,
        int                   k1,
        int                   s0,
        int                   s1,
        int                   p0,
        int                   p1) {

    bool is_node = false;

    if (a->grad) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    const int64_t ne[3] = {
        lm_ggml_calc_pool_output_size(a->ne[0], k0, s0, p0),
        lm_ggml_calc_pool_output_size(a->ne[1], k1, s1, p1),
        a->ne[2],
    };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, 3, ne);

    int32_t params[] = { op, k0, k1, s0, s1, p0, p1 };
    lm_ggml_set_op_params(result, params, sizeof(params));

    result->op = LM_GGML_OP_POOL_2D;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_upscale

static struct lm_ggml_tensor * lm_ggml_upscale_impl(
    struct lm_ggml_context * ctx,
    struct lm_ggml_tensor * a,
    int scale_factor) {
    bool is_node = false;

    if (a->grad) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    struct lm_ggml_tensor * result = lm_ggml_new_tensor_4d(ctx, a->type,
            a->ne[0] * scale_factor,
            a->ne[1] * scale_factor,
            a->ne[2], a->ne[3]);

    result->op = LM_GGML_OP_UPSCALE;
    result->op_params[0] = scale_factor;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = NULL;

    return result;
}

struct lm_ggml_tensor * lm_ggml_upscale(
    struct lm_ggml_context * ctx,
    struct lm_ggml_tensor * a,
    int scale_factor) {
    return lm_ggml_upscale_impl(ctx, a, scale_factor);
}

// lm_ggml_flash_attn

struct lm_ggml_tensor * lm_ggml_flash_attn(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * q,
        struct lm_ggml_tensor  * k,
        struct lm_ggml_tensor  * v,
        bool                  masked) {
    LM_GGML_ASSERT(lm_ggml_can_mul_mat(k, q));
    // TODO: check if vT can be multiplied by (k*qT)

    bool is_node = false;

    if (q->grad || k->grad || v->grad) {
        is_node = true;
    }

    //struct lm_ggml_tensor * result = lm_ggml_dup_tensor(ctx, q);
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, q->n_dims, q->ne);

    int32_t t = masked ? 1 : 0;
    lm_ggml_set_op_params(result, &t, sizeof(t));

    result->op   = LM_GGML_OP_FLASH_ATTN;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = q;
    result->src[1] = k;
    result->src[2] = v;

    return result;
}

// lm_ggml_flash_ff

struct lm_ggml_tensor * lm_ggml_flash_ff(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * b0,
        struct lm_ggml_tensor  * b1,
        struct lm_ggml_tensor  * c0,
        struct lm_ggml_tensor  * c1) {
    LM_GGML_ASSERT(lm_ggml_can_mul_mat(b0, a));
    // TODO: more checks

    bool is_node = false;

    if (a->grad || b0->grad || b1->grad || c0->grad || c1->grad) {
        is_node = true;
    }

    //struct lm_ggml_tensor * result = lm_ggml_dup_tensor(ctx, a);
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, a->n_dims, a->ne);

    result->op   = LM_GGML_OP_FLASH_FF;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b0;
    result->src[2] = b1;
    result->src[3] = c0;
    result->src[4] = c1;

    return result;
}

// lm_ggml_flash_attn_back

struct lm_ggml_tensor * lm_ggml_flash_attn_back(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * q,
        struct lm_ggml_tensor  * k,
        struct lm_ggml_tensor  * v,
        struct lm_ggml_tensor  * d,
        bool                  masked) {
    LM_GGML_ASSERT(lm_ggml_can_mul_mat(k, q));
    // TODO: check if vT can be multiplied by (k*qT)

    // d shape [D,N,ne2,ne3]
    // q shape [D,N,ne2,ne3]
    // k shape [D,M,kvne2,ne3]
    // v shape [M,D,kvne2,ne3]

    const int64_t     D = q->ne[0];
    const int64_t     N = q->ne[1];
    const int64_t     M = k->ne[1];
    const int64_t   ne2 = q->ne[2];
    const int64_t   ne3 = q->ne[3];
    const int64_t kvne2 = k->ne[2];

    LM_GGML_ASSERT(k->ne[0] == D);
    LM_GGML_ASSERT(v->ne[0] == M);
    LM_GGML_ASSERT(v->ne[1] == D);
    LM_GGML_ASSERT(d->ne[0] == D);
    LM_GGML_ASSERT(d->ne[1] == N);
    LM_GGML_ASSERT(k->ne[2] == kvne2);
    LM_GGML_ASSERT(k->ne[3] == ne3);
    LM_GGML_ASSERT(v->ne[2] == kvne2);
    LM_GGML_ASSERT(v->ne[3] == ne3);
    LM_GGML_ASSERT(d->ne[2] == ne2);
    LM_GGML_ASSERT(d->ne[3] == ne3);

    LM_GGML_ASSERT(ne2 % kvne2 == 0);

    bool is_node = false;

    if (q->grad || k->grad || v->grad) {
        // when using this operation (in backwards pass) these grads are set.
        // we don't want to create (big) grad of our result, so is_node is false.
        is_node = false;
    }

    // store gradients of q, k and v as continuous tensors concatenated in result.
    // note: v and gradv are actually transposed, i.e. v->ne[0] != D.
    const int64_t elem_q = lm_ggml_nelements(q);
    const int64_t elem_k = lm_ggml_nelements(k);
    const int64_t elem_v = lm_ggml_nelements(v);

    enum lm_ggml_type result_type = LM_GGML_TYPE_F32;
    LM_GGML_ASSERT(lm_ggml_blck_size(result_type) == 1);
    const size_t tsize = lm_ggml_type_size(result_type);

    const size_t offs_q = 0;
    const size_t offs_k = offs_q + LM_GGML_PAD(elem_q * tsize, LM_GGML_MEM_ALIGN);
    const size_t offs_v = offs_k + LM_GGML_PAD(elem_k * tsize, LM_GGML_MEM_ALIGN);
    const size_t end    = offs_v + LM_GGML_PAD(elem_v * tsize, LM_GGML_MEM_ALIGN);

    const size_t nelements = (end + tsize - 1)/tsize;

    struct lm_ggml_tensor * result = lm_ggml_new_tensor_1d(ctx, LM_GGML_TYPE_F32, nelements);

    int32_t masked_i = masked ? 1 : 0;
    lm_ggml_set_op_params(result, &masked_i, sizeof(masked_i));

    result->op   = LM_GGML_OP_FLASH_ATTN_BACK;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = q;
    result->src[1] = k;
    result->src[2] = v;
    result->src[3] = d;

    return result;
}

// lm_ggml_win_part

struct lm_ggml_tensor * lm_ggml_win_part(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int                   w) {
    LM_GGML_ASSERT(a->ne[3] == 1);
    LM_GGML_ASSERT(a->type  == LM_GGML_TYPE_F32);

    bool is_node = false;

    if (a->grad) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    // padding
    const int px = (w - a->ne[1]%w)%w;
    const int py = (w - a->ne[2]%w)%w;

    const int npx = (px + a->ne[1])/w;
    const int npy = (py + a->ne[2])/w;
    const int np  = npx*npy;

    const int64_t ne[4] = { a->ne[0], w, w, np, };

    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, 4, ne);

    int32_t params[] = { npx, npy, w };
    lm_ggml_set_op_params(result, params, sizeof(params));

    result->op   = LM_GGML_OP_WIN_PART;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_win_unpart

struct lm_ggml_tensor * lm_ggml_win_unpart(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int                   w0,
        int                   h0,
        int                   w) {
    LM_GGML_ASSERT(a->type == LM_GGML_TYPE_F32);

    bool is_node = false;

    if (a->grad) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    const int64_t ne[4] = { a->ne[0], w0, h0, 1, };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F32, 3, ne);

    int32_t params[] = { w };
    lm_ggml_set_op_params(result, params, sizeof(params));

    result->op   = LM_GGML_OP_WIN_UNPART;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

// lm_ggml_get_rel_pos

struct lm_ggml_tensor * lm_ggml_get_rel_pos(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        int                   qh,
        int                   kh) {
    LM_GGML_ASSERT(qh == kh);
    LM_GGML_ASSERT(2*MAX(qh, kh) - 1 == a->ne[1]);

    bool is_node = false;

    if (a->grad) {
        LM_GGML_ASSERT(false); // TODO: implement backward
        is_node = true;
    }

    const int64_t ne[4] = { a->ne[0], kh, qh, 1, };
    struct lm_ggml_tensor * result = lm_ggml_new_tensor(ctx, LM_GGML_TYPE_F16, 3, ne);

    result->op   = LM_GGML_OP_GET_REL_POS;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = NULL;

    return result;
}

// lm_ggml_add_rel_pos

static struct lm_ggml_tensor * lm_ggml_add_rel_pos_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * pw,
        struct lm_ggml_tensor  * ph,
        bool                  inplace) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(pw, ph));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(a));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(pw));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(ph));
    LM_GGML_ASSERT(ph->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT(pw->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT(pw->ne[3] == a->ne[2]);
    LM_GGML_ASSERT(pw->ne[0]*pw->ne[0] == a->ne[0]);
    LM_GGML_ASSERT(pw->ne[1]*pw->ne[2] == a->ne[1]);

    bool is_node = false;

    if (!inplace && (a->grad || pw->grad || ph->grad)) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);
    lm_ggml_set_op_params_i32(result, 0, inplace ? 1 : 0);

    result->op   = LM_GGML_OP_ADD_REL_POS;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = pw;
    result->src[2] = ph;

    return result;
}

struct lm_ggml_tensor * lm_ggml_add_rel_pos(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * pw,
        struct lm_ggml_tensor  * ph) {
    return lm_ggml_add_rel_pos_impl(ctx, a, pw, ph, false);
}

struct lm_ggml_tensor * lm_ggml_add_rel_pos_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        struct lm_ggml_tensor  * pw,
        struct lm_ggml_tensor  * ph) {
    return lm_ggml_add_rel_pos_impl(ctx, a, pw, ph, true);
}

// gmml_unary

static struct lm_ggml_tensor * lm_ggml_unary_impl(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * a,
        enum lm_ggml_unary_op op,
        bool inplace) {
    bool is_node = false;

    if (!inplace && (a->grad)) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    lm_ggml_set_op_params_i32(result, 0, (int32_t) op);

    result->op   = LM_GGML_OP_UNARY;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_unary(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        enum lm_ggml_unary_op op) {
    return lm_ggml_unary_impl(ctx, a, op, false);
}

struct lm_ggml_tensor * lm_ggml_unary_inplace(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor  * a,
        enum lm_ggml_unary_op op) {
    return lm_ggml_unary_impl(ctx, a, op, true);
}

// lm_ggml_map_unary

static struct lm_ggml_tensor * lm_ggml_map_unary_impl_f32(
        struct lm_ggml_context        * ctx,
        struct lm_ggml_tensor         * a,
        const  lm_ggml_unary_op_f32_t fun,
        bool   inplace) {
    bool is_node = false;

    if (!inplace && a->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    lm_ggml_set_op_params(result, (const void *) &fun, sizeof(fun));

    result->op = LM_GGML_OP_MAP_UNARY;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_map_unary_f32(
        struct lm_ggml_context        * ctx,
        struct lm_ggml_tensor         * a,
        const  lm_ggml_unary_op_f32_t fun) {
    return lm_ggml_map_unary_impl_f32(ctx, a, fun, false);
}

struct lm_ggml_tensor * lm_ggml_map_unary_inplace_f32(
        struct lm_ggml_context        * ctx,
        struct lm_ggml_tensor         * a,
        const  lm_ggml_unary_op_f32_t fun) {
    return lm_ggml_map_unary_impl_f32(ctx, a, fun, true);
}

// lm_ggml_map_binary

static struct lm_ggml_tensor * lm_ggml_map_binary_impl_f32(
        struct lm_ggml_context         * ctx,
        struct lm_ggml_tensor          * a,
        struct lm_ggml_tensor          * b,
        const  lm_ggml_binary_op_f32_t fun,
        bool   inplace) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(a, b));

    bool is_node = false;

    if (!inplace && (a->grad || b->grad)) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    lm_ggml_set_op_params(result, (const void *) &fun, sizeof(fun));

    result->op = LM_GGML_OP_MAP_BINARY;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_map_binary_f32(
        struct lm_ggml_context         * ctx,
        struct lm_ggml_tensor          * a,
        struct lm_ggml_tensor          * b,
        const  lm_ggml_binary_op_f32_t fun) {
    return lm_ggml_map_binary_impl_f32(ctx, a, b, fun, false);
}

struct lm_ggml_tensor * lm_ggml_map_binary_inplace_f32(
        struct lm_ggml_context         * ctx,
        struct lm_ggml_tensor          * a,
        struct lm_ggml_tensor          * b,
        const  lm_ggml_binary_op_f32_t fun) {
    return lm_ggml_map_binary_impl_f32(ctx, a, b, fun, true);
}

// lm_ggml_map_custom1_f32

static struct lm_ggml_tensor * lm_ggml_map_custom1_impl_f32(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        const  lm_ggml_custom1_op_f32_t   fun,
        bool   inplace) {
    bool is_node = false;

    if (!inplace && a->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    lm_ggml_set_op_params(result, (const void *) &fun, sizeof(fun));

    result->op = LM_GGML_OP_MAP_CUSTOM1_F32;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_map_custom1_f32(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        const  lm_ggml_custom1_op_f32_t   fun) {
    return lm_ggml_map_custom1_impl_f32(ctx, a, fun, false);
}

struct lm_ggml_tensor * lm_ggml_map_custom1_inplace_f32(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        const  lm_ggml_custom1_op_f32_t   fun) {
    return lm_ggml_map_custom1_impl_f32(ctx, a, fun, true);
}

// lm_ggml_map_custom2_f32

static struct lm_ggml_tensor * lm_ggml_map_custom2_impl_f32(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        struct lm_ggml_tensor           * b,
        const  lm_ggml_custom2_op_f32_t   fun,
        bool   inplace) {
    bool is_node = false;

    if (!inplace && (a->grad || b->grad)) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    lm_ggml_set_op_params(result, (const void *) &fun, sizeof(fun));

    result->op = LM_GGML_OP_MAP_CUSTOM2_F32;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_map_custom2_f32(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        struct lm_ggml_tensor           * b,
        const  lm_ggml_custom2_op_f32_t   fun) {
    return lm_ggml_map_custom2_impl_f32(ctx, a, b, fun, false);
}

struct lm_ggml_tensor * lm_ggml_map_custom2_inplace_f32(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        struct lm_ggml_tensor           * b,
        const  lm_ggml_custom2_op_f32_t   fun) {
    return lm_ggml_map_custom2_impl_f32(ctx, a, b, fun, true);
}

// lm_ggml_map_custom3_f32

static struct lm_ggml_tensor * lm_ggml_map_custom3_impl_f32(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        struct lm_ggml_tensor           * b,
        struct lm_ggml_tensor           * c,
        const  lm_ggml_custom3_op_f32_t   fun,
        bool   inplace) {
    bool is_node = false;

    if (!inplace && (a->grad || b->grad || c->grad)) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    lm_ggml_set_op_params(result, (const void *) &fun, sizeof(fun));

    result->op = LM_GGML_OP_MAP_CUSTOM3_F32;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;
    result->src[2] = c;

    return result;
}

struct lm_ggml_tensor * lm_ggml_map_custom3_f32(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        struct lm_ggml_tensor           * b,
        struct lm_ggml_tensor           * c,
        const  lm_ggml_custom3_op_f32_t   fun) {
    return lm_ggml_map_custom3_impl_f32(ctx, a, b, c, fun, false);
}

struct lm_ggml_tensor * lm_ggml_map_custom3_inplace_f32(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        struct lm_ggml_tensor           * b,
        struct lm_ggml_tensor           * c,
        const  lm_ggml_custom3_op_f32_t   fun) {
    return lm_ggml_map_custom3_impl_f32(ctx, a, b, c, fun, true);
}

// lm_ggml_map_custom1
struct lm_ggml_map_custom1_op_params {
    lm_ggml_custom1_op_t fun;
    int n_tasks;
    void * userdata;
};

static struct lm_ggml_tensor * lm_ggml_map_custom1_impl(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        const  lm_ggml_custom1_op_t       fun,
        int                            n_tasks,
        void                         * userdata,
        bool                           inplace) {
    LM_GGML_ASSERT(n_tasks == LM_GGML_N_TASKS_MAX || n_tasks > 0);

    bool is_node = false;

    if (!inplace && a->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    struct lm_ggml_map_custom1_op_params params = {
        /*.fun      =*/ fun,
        /*.n_tasks  =*/ n_tasks,
        /*.userdata =*/ userdata
    };
    lm_ggml_set_op_params(result, (const void *) &params, sizeof(params));

    result->op = LM_GGML_OP_MAP_CUSTOM1;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;

    return result;
}

struct lm_ggml_tensor * lm_ggml_map_custom1(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        const  lm_ggml_custom1_op_t       fun,
        int                            n_tasks,
        void                         * userdata) {
    return lm_ggml_map_custom1_impl(ctx, a, fun, n_tasks, userdata, false);
}

struct lm_ggml_tensor * lm_ggml_map_custom1_inplace(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        const  lm_ggml_custom1_op_t       fun,
        int                            n_tasks,
        void                         * userdata) {
    return lm_ggml_map_custom1_impl(ctx, a, fun, n_tasks, userdata, true);
}

// lm_ggml_map_custom2

struct lm_ggml_map_custom2_op_params {
    lm_ggml_custom2_op_t fun;
    int n_tasks;
    void * userdata;
};

static struct lm_ggml_tensor * lm_ggml_map_custom2_impl(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        struct lm_ggml_tensor           * b,
        const  lm_ggml_custom2_op_t       fun,
        int                            n_tasks,
        void                         * userdata,
        bool                           inplace) {
    LM_GGML_ASSERT(n_tasks == LM_GGML_N_TASKS_MAX || n_tasks > 0);

    bool is_node = false;

    if (!inplace && (a->grad || b->grad)) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    struct lm_ggml_map_custom2_op_params params = {
        /*.fun      =*/ fun,
        /*.n_tasks  =*/ n_tasks,
        /*.userdata =*/ userdata
    };
    lm_ggml_set_op_params(result, (const void *) &params, sizeof(params));

    result->op = LM_GGML_OP_MAP_CUSTOM2;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

struct lm_ggml_tensor * lm_ggml_map_custom2(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        struct lm_ggml_tensor           * b,
        const  lm_ggml_custom2_op_t       fun,
        int                            n_tasks,
        void                         * userdata) {
    return lm_ggml_map_custom2_impl(ctx, a, b, fun, n_tasks, userdata, false);
}

struct lm_ggml_tensor * lm_ggml_map_custom2_inplace(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        struct lm_ggml_tensor           * b,
        const  lm_ggml_custom2_op_t       fun,
        int                            n_tasks,
        void                         * userdata) {
    return lm_ggml_map_custom2_impl(ctx, a, b, fun, n_tasks, userdata, true);
}

// lm_ggml_map_custom3

struct lm_ggml_map_custom3_op_params {
    lm_ggml_custom3_op_t fun;
    int n_tasks;
    void * userdata;
};

static struct lm_ggml_tensor * lm_ggml_map_custom3_impl(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        struct lm_ggml_tensor           * b,
        struct lm_ggml_tensor           * c,
        const  lm_ggml_custom3_op_t       fun,
        int                            n_tasks,
        void                         * userdata,
        bool                           inplace) {
    LM_GGML_ASSERT(n_tasks == LM_GGML_N_TASKS_MAX || n_tasks > 0);

    bool is_node = false;

    if (!inplace && (a->grad || b->grad || c->grad)) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = inplace ? lm_ggml_view_tensor(ctx, a) : lm_ggml_dup_tensor(ctx, a);

    struct lm_ggml_map_custom3_op_params params = {
        /*.fun      =*/ fun,
        /*.n_tasks  =*/ n_tasks,
        /*.userdata =*/ userdata
    };
    lm_ggml_set_op_params(result, (const void *) &params, sizeof(params));

    result->op = LM_GGML_OP_MAP_CUSTOM3;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;
    result->src[2] = c;

    return result;
}

struct lm_ggml_tensor * lm_ggml_map_custom3(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        struct lm_ggml_tensor           * b,
        struct lm_ggml_tensor           * c,
        const  lm_ggml_custom3_op_t       fun,
        int                            n_tasks,
        void                         * userdata) {
    return lm_ggml_map_custom3_impl(ctx, a, b, c, fun, n_tasks, userdata, false);
}

struct lm_ggml_tensor * lm_ggml_map_custom3_inplace(
        struct lm_ggml_context          * ctx,
        struct lm_ggml_tensor           * a,
        struct lm_ggml_tensor           * b,
        struct lm_ggml_tensor           * c,
        const  lm_ggml_custom3_op_t       fun,
        int                            n_tasks,
        void                         * userdata) {
    return lm_ggml_map_custom3_impl(ctx, a, b, c, fun, n_tasks, userdata, true);
}

// lm_ggml_cross_entropy_loss

struct lm_ggml_tensor * lm_ggml_cross_entropy_loss(
        struct lm_ggml_context         * ctx,
        struct lm_ggml_tensor          * a,
        struct lm_ggml_tensor          * b) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(a, b));
    bool is_node = false;

    if (a->grad || b->grad) {
        is_node = true;
    }

    struct lm_ggml_tensor * result = lm_ggml_new_tensor_1d(ctx, a->type, 1);

    result->op   = LM_GGML_OP_CROSS_ENTROPY_LOSS;
    result->grad = is_node ? lm_ggml_dup_tensor(ctx, result) : NULL;
    result->src[0] = a;
    result->src[1] = b;

    return result;
}

// lm_ggml_cross_entropy_loss_back

struct lm_ggml_tensor * lm_ggml_cross_entropy_loss_back(
        struct lm_ggml_context         * ctx,
        struct lm_ggml_tensor          * a,
        struct lm_ggml_tensor          * b,
        struct lm_ggml_tensor          * c) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(a, b));
    LM_GGML_ASSERT(lm_ggml_is_scalar(c));

    struct lm_ggml_tensor * result = lm_ggml_dup_tensor(ctx, a);

    result->op   = LM_GGML_OP_CROSS_ENTROPY_LOSS_BACK;
    result->grad = NULL;
    result->src[0] = a;
    result->src[1] = b;
    result->src[2] = c;

    return result;
}

////////////////////////////////////////////////////////////////////////////////

void lm_ggml_set_param(
        struct lm_ggml_context * ctx,
        struct lm_ggml_tensor * tensor) {
    tensor->is_param = true;

    LM_GGML_ASSERT(tensor->grad == NULL);
    tensor->grad = lm_ggml_dup_tensor(ctx, tensor);
    lm_ggml_format_name(tensor->grad, "%s (grad)", tensor->name);
}

// lm_ggml_compute_forward_dup

static void lm_ggml_compute_forward_dup_same_cont(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_nelements(dst) == lm_ggml_nelements(src0));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(dst) && lm_ggml_is_contiguous(src0));
    LM_GGML_ASSERT(src0->type == dst->type);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const size_t nb00 = src0->nb[0];
    const size_t nb0 = dst->nb[0];

    const int ith = params->ith; // thread index
    const int nth = params->nth; // number of threads

    // parallelize by elements
    const int ne = lm_ggml_nelements(dst);
    const int dr = (ne + nth - 1) / nth;
    const int ie0 = dr * ith;
    const int ie1 = MIN(ie0 + dr, ne);

    if (ie0 < ie1) {
        memcpy(
            ((char *)  dst->data + ie0*nb0),
            ((char *) src0->data + ie0*nb00),
            (ie1 - ie0) * lm_ggml_type_size(src0->type));
    }

}
static void lm_ggml_compute_forward_dup_f16(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_nelements(dst) == lm_ggml_nelements(src0));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    const int ith = params->ith; // thread index
    const int nth = params->nth; // number of threads

    if (lm_ggml_is_contiguous(src0) && lm_ggml_is_contiguous(dst) && src0->type == dst->type) {
        lm_ggml_compute_forward_dup_same_cont(params, src0, dst);
        return;
    }

    // parallelize by rows
    const int nr = ne01;
    // number of rows per thread
    const int dr = (nr + nth - 1) / nth;
    // row range for this thread
    const int ir0 = dr * ith;
    const int ir1 = MIN(ir0 + dr, nr);

    if (src0->type == dst->type &&
        ne00 == ne0 &&
        nb00 == lm_ggml_type_size(src0->type) && nb0 == lm_ggml_type_size(dst->type)) {
        // copy by rows
        const size_t rs = ne00*nb00;
        for (int64_t i03 = 0; i03 < ne03; i03++) {
            for (int64_t i02 = 0; i02 < ne02; i02++) {
                for (int64_t i01 = ir0; i01 < ir1; i01++) {
                    memcpy(
                        ((char *)  dst->data + i01*nb1  + i02*nb2  + i03*nb3),
                        ((char *) src0->data + i01*nb01 + i02*nb02 + i03*nb03),
                        rs);
                }
            }
        }
        return;
    }

    // TODO: add more special-case implementations for tensor shapes/strides that can benefit from memcpy

    if (lm_ggml_is_contiguous(dst)) {
        if (nb00 == sizeof(lm_ggml_fp16_t)) {
            if (dst->type == LM_GGML_TYPE_F16) {
                size_t id = 0;
                const size_t rs = ne00 * nb00;
                char * dst_ptr = (char *) dst->data;

                for (int i03 = 0; i03 < ne03; i03++) {
                    for (int i02 = 0; i02 < ne02; i02++) {
                        id += rs * ir0;
                        for (int i01 = ir0; i01 < ir1; i01++) {
                            const char * src0_ptr = (char *) src0->data + i01*nb01 + i02*nb02 + i03*nb03;
                            memcpy(dst_ptr + id, src0_ptr, rs);
                            id += rs;
                        }
                        id += rs * (ne01 - ir1);
                    }
                }
            } else if (dst->type == LM_GGML_TYPE_F32) {
                size_t id = 0;
                float * dst_ptr = (float *) dst->data;

                for (int i03 = 0; i03 < ne03; i03++) {
                    for (int i02 = 0; i02 < ne02; i02++) {
                        id += ne00 * ir0;
                        for (int i01 = ir0; i01 < ir1; i01++) {
                            const lm_ggml_fp16_t * src0_ptr = (lm_ggml_fp16_t *) ((char *) src0->data + i01*nb01 + i02*nb02 + i03*nb03);
                            for (int i00 = 0; i00 < ne00; i00++) {
                                dst_ptr[id] = LM_GGML_FP16_TO_FP32(src0_ptr[i00]);
                                id++;
                            }
                        }
                        id += ne00 * (ne01 - ir1);
                    }
                }
            } else if (type_traits[dst->type].from_float) {
                lm_ggml_from_float_t const quantize_row_q = type_traits[dst->type].from_float;
                float * src0_f32 = (float *) params->wdata + (ne00 + CACHE_LINE_SIZE_F32) * ith;

                size_t id = 0;
                size_t rs = nb0 * (ne00 / lm_ggml_blck_size(dst->type));
                char * dst_ptr = (char *) dst->data;

                for (int i03 = 0; i03 < ne03; i03++) {
                    for (int i02 = 0; i02 < ne02; i02++) {
                        id += rs * ir0;
                        for (int i01 = ir0; i01 < ir1; i01++) {
                            const lm_ggml_fp16_t * src0_ptr = (lm_ggml_fp16_t *) ((char *) src0->data + i01*nb01 + i02*nb02 + i03*nb03);

                            for (int i00 = 0; i00 < ne00; i00++) {
                                src0_f32[i00] = LM_GGML_FP16_TO_FP32(src0_ptr[i00]);
                            }

                            quantize_row_q(src0_f32, dst_ptr + id, ne00);
                            id += rs;
                        }
                        id += rs * (ne01 - ir1);
                    }
                }
            } else {
                LM_GGML_ASSERT(false); // TODO: implement
            }
        } else {
            //printf("%s: this is not optimal - fix me\n", __func__);

            if (dst->type == LM_GGML_TYPE_F32) {
                size_t id = 0;
                float * dst_ptr = (float *) dst->data;

                for (int i03 = 0; i03 < ne03; i03++) {
                    for (int i02 = 0; i02 < ne02; i02++) {
                        id += ne00 * ir0;
                        for (int i01 = ir0; i01 < ir1; i01++) {
                            for (int i00 = 0; i00 < ne00; i00++) {
                                const lm_ggml_fp16_t * src0_ptr = (lm_ggml_fp16_t *) ((char *) src0->data + i00*nb00 + i01*nb01 + i02*nb02 + i03*nb03);

                                dst_ptr[id] = LM_GGML_FP16_TO_FP32(*src0_ptr);
                                id++;
                            }
                        }
                        id += ne00 * (ne01 - ir1);
                    }
                }
            } else if (dst->type == LM_GGML_TYPE_F16) {
                size_t id = 0;
                lm_ggml_fp16_t * dst_ptr = (lm_ggml_fp16_t *) dst->data;

                for (int i03 = 0; i03 < ne03; i03++) {
                    for (int i02 = 0; i02 < ne02; i02++) {
                        id += ne00 * ir0;
                        for (int i01 = ir0; i01 < ir1; i01++) {
                            for (int i00 = 0; i00 < ne00; i00++) {
                                const lm_ggml_fp16_t * src0_ptr = (lm_ggml_fp16_t *) ((char *) src0->data + i00*nb00 + i01*nb01 + i02*nb02 + i03*nb03);

                                dst_ptr[id] = *src0_ptr;
                                id++;
                            }
                        }
                        id += ne00 * (ne01 - ir1);
                    }
                }
            } else {
                LM_GGML_ASSERT(false); // TODO: implement
            }
        }
        return;
    }

    // dst counters
    int64_t i10 = 0;
    int64_t i11 = 0;
    int64_t i12 = 0;
    int64_t i13 = 0;

    if (dst->type == LM_GGML_TYPE_F16) {
        for (int64_t i03 = 0; i03 < ne03; i03++) {
            for (int64_t i02 = 0; i02 < ne02; i02++) {
                i10 += ne00 * ir0;
                while (i10 >= ne0) {
                    i10 -= ne0;
                    if (++i11 == ne1) {
                        i11 = 0;
                        if (++i12 == ne2) {
                            i12 = 0;
                            if (++i13 == ne3) {
                                i13 = 0;
                            }
                        }
                    }
                }
                for (int64_t i01 = ir0; i01 < ir1; i01++) {
                    for (int64_t i00 = 0; i00 < ne00; i00++) {
                        const char * src0_ptr = ((char *) src0->data + i00*nb00 + i01*nb01 + i02*nb02 + i03*nb03);
                              char * dst_ptr  = ((char *)  dst->data + i10*nb0  + i11*nb1  + i12*nb2  + i13*nb3);

                        memcpy(dst_ptr, src0_ptr, sizeof(lm_ggml_fp16_t));

                        if (++i10 == ne00) {
                            i10 = 0;
                            if (++i11 == ne01) {
                                i11 = 0;
                                if (++i12 == ne02) {
                                    i12 = 0;
                                    if (++i13 == ne03) {
                                        i13 = 0;
                                    }
                                }
                            }
                        }
                    }
                }
                i10 += ne00 * (ne01 - ir1);
                while (i10 >= ne0) {
                    i10 -= ne0;
                    if (++i11 == ne1) {
                        i11 = 0;
                        if (++i12 == ne2) {
                            i12 = 0;
                            if (++i13 == ne3) {
                                i13 = 0;
                            }
                        }
                    }
                }
            }
        }
    } else if (dst->type == LM_GGML_TYPE_F32) {
        for (int64_t i03 = 0; i03 < ne03; i03++) {
            for (int64_t i02 = 0; i02 < ne02; i02++) {
                i10 += ne00 * ir0;
                while (i10 >= ne0) {
                    i10 -= ne0;
                    if (++i11 == ne1) {
                        i11 = 0;
                        if (++i12 == ne2) {
                            i12 = 0;
                            if (++i13 == ne3) {
                                i13 = 0;
                            }
                        }
                    }
                }
                for (int64_t i01 = ir0; i01 < ir1; i01++) {
                    for (int64_t i00 = 0; i00 < ne00; i00++) {
                        const char * src0_ptr = ((char *) src0->data + i00*nb00 + i01*nb01 + i02*nb02 + i03*nb03);
                              char * dst_ptr  = ((char *)  dst->data + i10*nb0  + i11*nb1  + i12*nb2  + i13*nb3);

                        *(float *) dst_ptr = LM_GGML_FP16_TO_FP32(*(const lm_ggml_fp16_t *) src0_ptr);

                        if (++i10 == ne0) {
                            i10 = 0;
                            if (++i11 == ne1) {
                                i11 = 0;
                                if (++i12 == ne2) {
                                    i12 = 0;
                                    if (++i13 == ne3) {
                                        i13 = 0;
                                    }
                                }
                            }
                        }
                    }
                }
                i10 += ne00 * (ne01 - ir1);
                while (i10 >= ne0) {
                    i10 -= ne0;
                    if (++i11 == ne1) {
                        i11 = 0;
                        if (++i12 == ne2) {
                            i12 = 0;
                            if (++i13 == ne3) {
                                i13 = 0;
                            }
                        }
                    }
                }
            }
        }
    } else {
        LM_GGML_ASSERT(false); // TODO: implement
    }
}

static void lm_ggml_compute_forward_dup_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_nelements(dst) == lm_ggml_nelements(src0));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    const int ith = params->ith; // thread index
    const int nth = params->nth; // number of threads

    if (lm_ggml_is_contiguous(src0) && lm_ggml_is_contiguous(dst) && src0->type == dst->type) {
        lm_ggml_compute_forward_dup_same_cont(params, src0, dst);
        return;
    }

    // parallelize by rows
    const int nr = ne01;
    // number of rows per thread
    const int dr = (nr + nth - 1) / nth;
    // row range for this thread
    const int ir0 = dr * ith;
    const int ir1 = MIN(ir0 + dr, nr);

    if (src0->type == dst->type &&
        ne00 == ne0 &&
        nb00 == lm_ggml_type_size(src0->type) && nb0 == lm_ggml_type_size(dst->type)) {
        // copy by rows
        const size_t rs = ne00*nb00;
        for (int64_t i03 = 0; i03 < ne03; i03++) {
            for (int64_t i02 = 0; i02 < ne02; i02++) {
                for (int64_t i01 = ir0; i01 < ir1; i01++) {
                    memcpy(
                        ((char *)  dst->data + i01*nb1  + i02*nb2  + i03*nb3),
                        ((char *) src0->data + i01*nb01 + i02*nb02 + i03*nb03),
                        rs);
                }
            }
        }
        return;
    }

    if (lm_ggml_is_contiguous(dst)) {
        // TODO: simplify
        if (nb00 == sizeof(float)) {
            if (dst->type == LM_GGML_TYPE_F32) {
                size_t id = 0;
                const size_t rs = ne00 * nb00;
                char * dst_ptr = (char *) dst->data;

                for (int i03 = 0; i03 < ne03; i03++) {
                    for (int i02 = 0; i02 < ne02; i02++) {
                        id += rs * ir0;
                        for (int i01 = ir0; i01 < ir1; i01++) {
                            const char * src0_ptr = (char *) src0->data + i01*nb01 + i02*nb02 + i03*nb03;
                            memcpy(dst_ptr + id, src0_ptr, rs);
                            id += rs;
                        }
                        id += rs * (ne01 - ir1);
                    }
                }
            } else if (type_traits[dst->type].from_float) {
                lm_ggml_from_float_t const quantize_row_q = type_traits[dst->type].from_float;

                size_t id = 0;
                size_t rs = nb0 * (ne00 / lm_ggml_blck_size(dst->type));
                char * dst_ptr = (char *) dst->data;

                for (int i03 = 0; i03 < ne03; i03++) {
                    for (int i02 = 0; i02 < ne02; i02++) {
                        id += rs * ir0;
                        for (int i01 = ir0; i01 < ir1; i01++) {
                            const float * src0_ptr = (float *) ((char *) src0->data + i01*nb01 + i02*nb02 + i03*nb03);
                            quantize_row_q(src0_ptr, dst_ptr + id, ne00);
                            id += rs;
                        }
                        id += rs * (ne01 - ir1);
                    }
                }
            } else {
                LM_GGML_ASSERT(false); // TODO: implement
            }
        } else {
            //printf("%s: this is not optimal - fix me\n", __func__);

            if (dst->type == LM_GGML_TYPE_F32) {
                size_t id = 0;
                float * dst_ptr = (float *) dst->data;

                for (int i03 = 0; i03 < ne03; i03++) {
                    for (int i02 = 0; i02 < ne02; i02++) {
                        id += ne00 * ir0;
                        for (int i01 = ir0; i01 < ir1; i01++) {
                            for (int i00 = 0; i00 < ne00; i00++) {
                                const float * src0_ptr = (float *) ((char *) src0->data + i00*nb00 + i01*nb01 + i02*nb02 + i03*nb03);

                                dst_ptr[id] = *src0_ptr;
                                id++;
                            }
                        }
                        id += ne00 * (ne01 - ir1);
                    }
                }
            } else if (dst->type == LM_GGML_TYPE_F16) {
                size_t id = 0;
                lm_ggml_fp16_t * dst_ptr = (lm_ggml_fp16_t *) dst->data;

                for (int i03 = 0; i03 < ne03; i03++) {
                    for (int i02 = 0; i02 < ne02; i02++) {
                        id += ne00 * ir0;
                        for (int i01 = ir0; i01 < ir1; i01++) {
                            for (int i00 = 0; i00 < ne00; i00++) {
                                const float * src0_ptr = (float *) ((char *) src0->data + i00*nb00 + i01*nb01 + i02*nb02 + i03*nb03);

                                dst_ptr[id] = LM_GGML_FP32_TO_FP16(*src0_ptr);
                                id++;
                            }
                        }
                        id += ne00 * (ne01 - ir1);
                    }
                }
            } else {
                LM_GGML_ASSERT(false); // TODO: implement
            }
        }

        return;
    }

    // dst counters

    int64_t i10 = 0;
    int64_t i11 = 0;
    int64_t i12 = 0;
    int64_t i13 = 0;

    if (dst->type == LM_GGML_TYPE_F32) {
        for (int64_t i03 = 0; i03 < ne03; i03++) {
            for (int64_t i02 = 0; i02 < ne02; i02++) {
                i10 += ne00 * ir0;
                while (i10 >= ne0) {
                    i10 -= ne0;
                    if (++i11 == ne1) {
                        i11 = 0;
                        if (++i12 == ne2) {
                            i12 = 0;
                            if (++i13 == ne3) {
                                i13 = 0;
                            }
                        }
                    }
                }
                for (int64_t i01 = ir0; i01 < ir1; i01++) {
                    for (int64_t i00 = 0; i00 < ne00; i00++) {
                        const char * src0_ptr = ((char *) src0->data + i00*nb00 + i01*nb01 + i02*nb02 + i03*nb03);
                              char * dst_ptr  = ((char *)  dst->data + i10*nb0  + i11*nb1  + i12*nb2  + i13*nb3);

                        memcpy(dst_ptr, src0_ptr, sizeof(float));

                        if (++i10 == ne0) {
                            i10 = 0;
                            if (++i11 == ne1) {
                                i11 = 0;
                                if (++i12 == ne2) {
                                    i12 = 0;
                                    if (++i13 == ne3) {
                                        i13 = 0;
                                    }
                                }
                            }
                        }
                    }
                }
                i10 += ne00 * (ne01 - ir1);
                while (i10 >= ne0) {
                    i10 -= ne0;
                    if (++i11 == ne1) {
                        i11 = 0;
                        if (++i12 == ne2) {
                            i12 = 0;
                            if (++i13 == ne3) {
                                i13 = 0;
                            }
                        }
                    }
                }
            }
        }
    } else if (dst->type == LM_GGML_TYPE_F16) {
        for (int64_t i03 = 0; i03 < ne03; i03++) {
            for (int64_t i02 = 0; i02 < ne02; i02++) {
                i10 += ne00 * ir0;
                while (i10 >= ne0) {
                    i10 -= ne0;
                    if (++i11 == ne1) {
                        i11 = 0;
                        if (++i12 == ne2) {
                            i12 = 0;
                            if (++i13 == ne3) {
                                i13 = 0;
                            }
                        }
                    }
                }
                for (int64_t i01 = ir0; i01 < ir1; i01++) {
                    for (int64_t i00 = 0; i00 < ne00; i00++) {
                        const char * src0_ptr = ((char *) src0->data + i00*nb00 + i01*nb01 + i02*nb02 + i03*nb03);
                              char * dst_ptr  = ((char *)  dst->data + i10*nb0  + i11*nb1  + i12*nb2  + i13*nb3);

                        *(lm_ggml_fp16_t *) dst_ptr = LM_GGML_FP32_TO_FP16(*(const float *) src0_ptr);

                        if (++i10 == ne0) {
                            i10 = 0;
                            if (++i11 == ne1) {
                                i11 = 0;
                                if (++i12 == ne2) {
                                    i12 = 0;
                                    if (++i13 == ne3) {
                                        i13 = 0;
                                    }
                                }
                            }
                        }
                    }
                }
                i10 += ne00 * (ne01 - ir1);
                while (i10 >= ne0) {
                    i10 -= ne0;
                    if (++i11 == ne1) {
                        i11 = 0;
                        if (++i12 == ne2) {
                            i12 = 0;
                            if (++i13 == ne3) {
                                i13 = 0;
                            }
                        }
                    }
                }
            }
        }
    } else {
        LM_GGML_ASSERT(false); // TODO: implement
    }
}

static void lm_ggml_compute_forward_dup(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    if (lm_ggml_is_contiguous(src0) && lm_ggml_is_contiguous(dst) && src0->type == dst->type) {
        lm_ggml_compute_forward_dup_same_cont(params, src0, dst);
        return;
    }
    switch (src0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_dup_f16(params, src0, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_dup_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_add

static void lm_ggml_compute_forward_add_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_can_repeat_rows(src1, src0) && lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int ith = params->ith;
    const int nth = params->nth;

    const int nr  = lm_ggml_nrows(src0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    LM_GGML_ASSERT( nb0 == sizeof(float));
    LM_GGML_ASSERT(nb00 == sizeof(float));

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    if (nb10 == sizeof(float)) {
        for (int ir = ir0; ir < ir1; ++ir) {
            // src1 is broadcastable across src0 and dst in i1, i2, i3
            const int64_t i03 = ir/(ne02*ne01);
            const int64_t i02 = (ir - i03*ne02*ne01)/ne01;
            const int64_t i01 = (ir - i03*ne02*ne01 - i02*ne01);

            const int64_t i13 = i03 % ne13;
            const int64_t i12 = i02 % ne12;
            const int64_t i11 = i01 % ne11;

            float * dst_ptr  = (float *) ((char *) dst->data  + i03*nb3  + i02*nb2  + i01*nb1 );
            float * src0_ptr = (float *) ((char *) src0->data + i03*nb03 + i02*nb02 + i01*nb01);
            float * src1_ptr = (float *) ((char *) src1->data + i13*nb13 + i12*nb12 + i11*nb11);

#ifdef LM_GGML_USE_ACCELERATE
            vDSP_vadd(src0_ptr, 1, src1_ptr, 1, dst_ptr, 1, ne00);
#else
            lm_ggml_vec_add_f32(ne00, dst_ptr, src0_ptr, src1_ptr);
#endif
        }
    } else {
        // src1 is not contiguous
        for (int ir = ir0; ir < ir1; ++ir) {
            // src1 is broadcastable across src0 and dst in i1, i2, i3
            const int64_t i03 = ir/(ne02*ne01);
            const int64_t i02 = (ir - i03*ne02*ne01)/ne01;
            const int64_t i01 = (ir - i03*ne02*ne01 - i02*ne01);

            const int64_t i13 = i03 % ne13;
            const int64_t i12 = i02 % ne12;
            const int64_t i11 = i01 % ne11;

            float * dst_ptr  = (float *) ((char *) dst->data  + i03*nb3  + i02*nb2  + i01*nb1 );
            float * src0_ptr = (float *) ((char *) src0->data + i03*nb03 + i02*nb02 + i01*nb01);

            for (int i0 = 0; i0 < ne0; i0++) {
                float * src1_ptr = (float *) ((char *) src1->data + i13*nb13 + i12*nb12 + i11*nb11 + i0*nb10);

                dst_ptr[i0] = src0_ptr[i0] + *src1_ptr;
            }
        }
    }
}

static void lm_ggml_compute_forward_add_f16_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, src1) && lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int ith = params->ith;
    const int nth = params->nth;

    const int nr  = lm_ggml_nrows(src0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    LM_GGML_ASSERT(src0->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT(dst->type  == LM_GGML_TYPE_F16);

    LM_GGML_ASSERT( nb0 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb00 == sizeof(lm_ggml_fp16_t));

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    if (nb10 == sizeof(float)) {
        for (int ir = ir0; ir < ir1; ++ir) {
            // src0, src1 and dst are same shape => same indices
            const int i3 = ir/(ne2*ne1);
            const int i2 = (ir - i3*ne2*ne1)/ne1;
            const int i1 = (ir - i3*ne2*ne1 - i2*ne1);

            lm_ggml_fp16_t * dst_ptr  = (lm_ggml_fp16_t *) ((char *) dst->data  + i3*nb3  + i2*nb2  + i1*nb1);
            lm_ggml_fp16_t * src0_ptr = (lm_ggml_fp16_t *) ((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01);
            float *       src1_ptr = (float *)       ((char *) src1->data + i3*nb13 + i2*nb12 + i1*nb11);

            for (int i = 0; i < ne0; i++) {
                dst_ptr[i] = LM_GGML_FP32_TO_FP16(LM_GGML_FP16_TO_FP32(src0_ptr[i]) + src1_ptr[i]);
            }
        }
    }
    else {
        // src1 is not contiguous
        LM_GGML_ASSERT(false);
    }
}

static void lm_ggml_compute_forward_add_f16_f16(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, src1) && lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int ith = params->ith;
    const int nth = params->nth;

    const int nr  = lm_ggml_nrows(src0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    LM_GGML_ASSERT(src0->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT(dst->type  == LM_GGML_TYPE_F16);

    LM_GGML_ASSERT( nb0 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb00 == sizeof(lm_ggml_fp16_t));

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    if (nb10 == sizeof(lm_ggml_fp16_t)) {
        for (int ir = ir0; ir < ir1; ++ir) {
            // src0, src1 and dst are same shape => same indices
            const int i3 = ir/(ne2*ne1);
            const int i2 = (ir - i3*ne2*ne1)/ne1;
            const int i1 = (ir - i3*ne2*ne1 - i2*ne1);

            lm_ggml_fp16_t * dst_ptr  = (lm_ggml_fp16_t *) ((char *) dst->data  + i3*nb3  + i2*nb2  + i1*nb1);
            lm_ggml_fp16_t * src0_ptr = (lm_ggml_fp16_t *) ((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01);
            lm_ggml_fp16_t * src1_ptr = (lm_ggml_fp16_t *) ((char *) src1->data + i3*nb13 + i2*nb12 + i1*nb11);

            for (int i = 0; i < ne0; i++) {
                dst_ptr[i] = LM_GGML_FP32_TO_FP16(LM_GGML_FP16_TO_FP32(src0_ptr[i]) + LM_GGML_FP16_TO_FP32(src1_ptr[i]));
            }
        }
    }
    else {
        // src1 is not contiguous
        LM_GGML_ASSERT(false);
    }
}

static void lm_ggml_compute_forward_add_q_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, src1) && lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int nr  = lm_ggml_nrows(src0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    const int ith = params->ith;
    const int nth = params->nth;

    const enum lm_ggml_type type = src0->type;
    const enum lm_ggml_type dtype = dst->type;
    lm_ggml_to_float_t const dequantize_row_q = type_traits[type].to_float;
    lm_ggml_from_float_t const quantize_row_q = type_traits[dtype].from_float;

    // we don't support permuted src0 or src1
    LM_GGML_ASSERT(nb00 == lm_ggml_type_size(type));
    LM_GGML_ASSERT(nb10 == sizeof(float));

    // dst cannot be transposed or permuted
    LM_GGML_ASSERT(nb0 <= nb1);
    LM_GGML_ASSERT(nb1 <= nb2);
    LM_GGML_ASSERT(nb2 <= nb3);

    LM_GGML_ASSERT(lm_ggml_is_quantized(src0->type));
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F32);

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    float * wdata = (float *) params->wdata + (ne00 + CACHE_LINE_SIZE_F32) * ith;

    for (int ir = ir0; ir < ir1; ++ir) {
        // src0 indices
        const int i03 = ir/(ne02*ne01);
        const int i02 = (ir - i03*ne02*ne01)/ne01;
        const int i01 = (ir - i03*ne02*ne01 - i02*ne01);

        // src1 and dst are same shape as src0 => same indices
        const int i13 = i03;
        const int i12 = i02;
        const int i11 = i01;

        const int i3 = i03;
        const int i2 = i02;
        const int i1 = i01;

        void  * src0_row = (void *) ((char *) src0->data + (i01*nb01 + i02*nb02 + i03*nb03));
        float * src1_row = (float *)((char *) src1->data + (i11*nb11 + i12*nb12 + i13*nb13));
        void  * dst_row  = (void *) ((char *)  dst->data + ( i1*nb1  +  i2*nb2  +  i3*nb3));

        assert(ne00 % 32 == 0);

        // unquantize row from src0 to temp buffer
        dequantize_row_q(src0_row, wdata, ne00);
        // add src1
        lm_ggml_vec_acc_f32(ne00, wdata, src1_row);
        // quantize row to dst
        if (quantize_row_q != NULL) {
            quantize_row_q(wdata, dst_row, ne00);
        } else {
            memcpy(dst_row, wdata, ne0*nb0);
        }
    }
}

static void lm_ggml_compute_forward_add(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_add_f32(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F16:
            {
                if (src1->type == LM_GGML_TYPE_F16) {
                    lm_ggml_compute_forward_add_f16_f16(params, src0, src1, dst);
                }
                else if (src1->type == LM_GGML_TYPE_F32) {
                    lm_ggml_compute_forward_add_f16_f32(params, src0, src1, dst);
                }
                else {
                    LM_GGML_ASSERT(false);
                }
            } break;
        case LM_GGML_TYPE_Q4_0:
        case LM_GGML_TYPE_Q4_1:
        case LM_GGML_TYPE_Q5_0:
        case LM_GGML_TYPE_Q5_1:
        case LM_GGML_TYPE_Q8_0:
        case LM_GGML_TYPE_Q2_K:
        case LM_GGML_TYPE_Q3_K:
        case LM_GGML_TYPE_Q4_K:
        case LM_GGML_TYPE_Q5_K:
        case LM_GGML_TYPE_Q6_K:
            {
                lm_ggml_compute_forward_add_q_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_add1

static void lm_ggml_compute_forward_add1_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));
    LM_GGML_ASSERT(lm_ggml_is_scalar(src1));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int ith = params->ith;
    const int nth = params->nth;

    const int nr  = lm_ggml_nrows(src0);

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    LM_GGML_ASSERT( nb0 == sizeof(float));
    LM_GGML_ASSERT(nb00 == sizeof(float));

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    for (int ir = ir0; ir < ir1; ++ir) {
        // src0 and dst are same shape => same indices
        const int i3 = ir/(ne2*ne1);
        const int i2 = (ir - i3*ne2*ne1)/ne1;
        const int i1 = (ir - i3*ne2*ne1 - i2*ne1);

#ifdef LM_GGML_USE_ACCELERATE
        UNUSED(lm_ggml_vec_add1_f32);

        vDSP_vadd(
                (float *) ((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01), 1,
                (float *) ((char *) src1->data), 0,
                (float *) ((char *) dst->data  + i3*nb3  + i2*nb2  + i1*nb1 ), 1,
                ne0);
#else
        lm_ggml_vec_add1_f32(ne0,
                (float *) ((char *) dst->data  + i3*nb3  + i2*nb2  + i1*nb1 ),
                (float *) ((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01),
               *(float *) src1->data);
#endif
    }
}

static void lm_ggml_compute_forward_add1_f16_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));
    LM_GGML_ASSERT(lm_ggml_is_scalar(src1));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // scalar to add
    const float v = *(float *) src1->data;

    const int ith = params->ith;
    const int nth = params->nth;

    const int nr  = lm_ggml_nrows(src0);

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    LM_GGML_ASSERT(src0->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT(dst->type  == LM_GGML_TYPE_F16);

    LM_GGML_ASSERT( nb0 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb00 == sizeof(lm_ggml_fp16_t));

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    for (int ir = ir0; ir < ir1; ++ir) {
        // src0 and dst are same shape => same indices
        const int i3 = ir/(ne2*ne1);
        const int i2 = (ir - i3*ne2*ne1)/ne1;
        const int i1 = (ir - i3*ne2*ne1 - i2*ne1);

        lm_ggml_fp16_t * dst_ptr  = (lm_ggml_fp16_t *) ((char *) dst->data  + i3*nb3  + i2*nb2  + i1*nb1 );
        lm_ggml_fp16_t * src0_ptr = (lm_ggml_fp16_t *) ((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01);
        for (int i = 0; i < ne0; i++) {
            dst_ptr[i] = LM_GGML_FP32_TO_FP16(LM_GGML_FP16_TO_FP32(src0_ptr[i]) + v);
        }
    }
}

static void lm_ggml_compute_forward_add1_f16_f16(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));
    LM_GGML_ASSERT(lm_ggml_is_scalar(src1));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // scalar to add
    const float v = LM_GGML_FP16_TO_FP32(*(lm_ggml_fp16_t *) src1->data);

    const int ith = params->ith;
    const int nth = params->nth;

    const int nr  = lm_ggml_nrows(src0);

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    LM_GGML_ASSERT(src0->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT(dst->type  == LM_GGML_TYPE_F16);

    LM_GGML_ASSERT( nb0 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb00 == sizeof(lm_ggml_fp16_t));

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    for (int ir = ir0; ir < ir1; ++ir) {
        // src0 and dst are same shape => same indices
        const int i3 = ir/(ne2*ne1);
        const int i2 = (ir - i3*ne2*ne1)/ne1;
        const int i1 = (ir - i3*ne2*ne1 - i2*ne1);

        lm_ggml_fp16_t * dst_ptr  = (lm_ggml_fp16_t *) ((char *) dst->data  + i3*nb3  + i2*nb2  + i1*nb1 );
        lm_ggml_fp16_t * src0_ptr = (lm_ggml_fp16_t *) ((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01);
        for (int i = 0; i < ne0; i++) {
            dst_ptr[i] = LM_GGML_FP32_TO_FP16(LM_GGML_FP16_TO_FP32(src0_ptr[i]) + v);
        }
    }
}

static void lm_ggml_compute_forward_add1_q_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));
    LM_GGML_ASSERT(lm_ggml_is_scalar(src1));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // scalar to add
    const float v = *(float *) src1->data;

    const int ith = params->ith;
    const int nth = params->nth;

    const int nr  = lm_ggml_nrows(src0);

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    const enum lm_ggml_type type = src0->type;
    lm_ggml_to_float_t const dequantize_row_q = type_traits[type].to_float;
    lm_ggml_from_float_t const quantize_row_q = type_traits[type].from_float;

    // we don't support permuted src0
    LM_GGML_ASSERT(nb00 == lm_ggml_type_size(type));

    // dst cannot be transposed or permuted
    LM_GGML_ASSERT(nb0 <= nb1);
    LM_GGML_ASSERT(nb1 <= nb2);
    LM_GGML_ASSERT(nb2 <= nb3);

    LM_GGML_ASSERT(lm_ggml_is_quantized(src0->type));
    LM_GGML_ASSERT(dst->type == src0->type);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F32);

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    float * wdata = (float *) params->wdata + (ne0 + CACHE_LINE_SIZE_F32) * ith;

    for (int ir = ir0; ir < ir1; ++ir) {
        // src0 and dst are same shape => same indices
        const int i3 = ir/(ne2*ne1);
        const int i2 = (ir - i3*ne2*ne1)/ne1;
        const int i1 = (ir - i3*ne2*ne1 - i2*ne1);

        void  * src0_row = (void *) ((char *) src0->data + (i1*nb01 + i2*nb02 + i3*nb03));
        void  * dst_row  = (void *) ((char *)  dst->data + (i1*nb1  + i2*nb2  + i3*nb0 ));

        assert(ne0 % 32 == 0);

        // unquantize row from src0 to temp buffer
        dequantize_row_q(src0_row, wdata, ne0);
        // add src1
        lm_ggml_vec_acc1_f32(ne0, wdata, v);
        // quantize row to dst
        quantize_row_q(wdata, dst_row, ne0);
    }
}

static void lm_ggml_compute_forward_add1(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_add1_f32(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F16:
            {
                if (src1->type == LM_GGML_TYPE_F16) {
                    lm_ggml_compute_forward_add1_f16_f16(params, src0, src1, dst);
                }
                else if (src1->type == LM_GGML_TYPE_F32) {
                    lm_ggml_compute_forward_add1_f16_f32(params, src0, src1, dst);
                }
                else {
                    LM_GGML_ASSERT(false);
                }
            } break;
        case LM_GGML_TYPE_Q4_0:
        case LM_GGML_TYPE_Q4_1:
        case LM_GGML_TYPE_Q5_0:
        case LM_GGML_TYPE_Q5_1:
        case LM_GGML_TYPE_Q8_0:
        case LM_GGML_TYPE_Q8_1:
        case LM_GGML_TYPE_Q2_K:
        case LM_GGML_TYPE_Q3_K:
        case LM_GGML_TYPE_Q4_K:
        case LM_GGML_TYPE_Q5_K:
        case LM_GGML_TYPE_Q6_K:
            {
                lm_ggml_compute_forward_add1_q_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_acc

static void lm_ggml_compute_forward_acc_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(dst) && lm_ggml_is_contiguous(src0));

    // view src0 and dst with these strides and data offset inbytes during acc
    // nb0 is implicitely element_size because src0 and dst are contiguous
    size_t nb1     = ((int32_t *) dst->op_params)[0];
    size_t nb2     = ((int32_t *) dst->op_params)[1];
    size_t nb3     = ((int32_t *) dst->op_params)[2];
    size_t offset  = ((int32_t *) dst->op_params)[3];
    bool   inplace = (bool) ((int32_t *) dst->op_params)[4];

    if (!inplace && (params->type == LM_GGML_TASK_INIT)) {
        // memcpy needs to be synchronized across threads to avoid race conditions.
        // => do it in INIT phase
        memcpy(
            ((char *)  dst->data),
            ((char *) src0->data),
            lm_ggml_nbytes(dst));
    }

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int ith = params->ith;
    const int nth = params->nth;

    const int nr = lm_ggml_nrows(src1);
    const int nc = src1->ne[0];

    LM_GGML_TENSOR_LOCALS(int64_t, ne1, src1, ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nb1, src1, nb)

    // src0 and dst as viewed during acc
    const size_t nb0 = lm_ggml_element_size(src0);

    const size_t nb00 = nb0;
    const size_t nb01 = nb1;
    const size_t nb02 = nb2;
    const size_t nb03 = nb3;

    LM_GGML_ASSERT(offset + (ne10 == 0 ? 0 : ne10-1)*nb0  + (ne11 == 0 ? 0 : ne11-1)*nb1  + (ne12 == 0 ? 0 : ne12-1)*nb2  + (ne13 == 0 ? 0 : ne13-1)*nb3  < lm_ggml_nbytes(dst));
    LM_GGML_ASSERT(offset + (ne10 == 0 ? 0 : ne10-1)*nb00 + (ne11 == 0 ? 0 : ne11-1)*nb01 + (ne12 == 0 ? 0 : ne12-1)*nb02 + (ne13 == 0 ? 0 : ne13-1)*nb03 < lm_ggml_nbytes(src0));

    LM_GGML_ASSERT(nb10 == sizeof(float));

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    for (int ir = ir0; ir < ir1; ++ir) {
        // src0 and dst are viewed with shape of src1 and offset
        // => same indices
        const int i3 = ir/(ne12*ne11);
        const int i2 = (ir - i3*ne12*ne11)/ne11;
        const int i1 = (ir - i3*ne12*ne11 - i2*ne11);

#ifdef LM_GGML_USE_ACCELERATE
        vDSP_vadd(
                (float *) ((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01 + offset), 1,
                (float *) ((char *) src1->data + i3*nb13 + i2*nb12 + i1*nb11), 1,
                (float *) ((char *) dst->data  + i3*nb3  + i2*nb2  + i1*nb1  + offset), 1, nc);
#else
        lm_ggml_vec_add_f32(nc,
                (float *) ((char *)  dst->data + i3*nb3  + i2*nb2  + i1*nb1  + offset),
                (float *) ((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01 + offset),
                (float *) ((char *) src1->data + i3*nb13 + i2*nb12 + i1*nb11));
#endif
    }
}

static void lm_ggml_compute_forward_acc(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {

    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_acc_f32(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F16:
        case LM_GGML_TYPE_Q4_0:
        case LM_GGML_TYPE_Q4_1:
        case LM_GGML_TYPE_Q5_0:
        case LM_GGML_TYPE_Q5_1:
        case LM_GGML_TYPE_Q8_0:
        case LM_GGML_TYPE_Q8_1:
        case LM_GGML_TYPE_Q2_K:
        case LM_GGML_TYPE_Q3_K:
        case LM_GGML_TYPE_Q4_K:
        case LM_GGML_TYPE_Q5_K:
        case LM_GGML_TYPE_Q6_K:
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_sub

static void lm_ggml_compute_forward_sub_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);
    assert(lm_ggml_are_same_shape(src0, src1) && lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int nr  = lm_ggml_nrows(src0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    LM_GGML_ASSERT( nb0 == sizeof(float));
    LM_GGML_ASSERT(nb00 == sizeof(float));

    if (nb10 == sizeof(float)) {
        for (int ir = 0; ir < nr; ++ir) {
            // src0, src1 and dst are same shape => same indices
            const int i3 = ir/(ne2*ne1);
            const int i2 = (ir - i3*ne2*ne1)/ne1;
            const int i1 = (ir - i3*ne2*ne1 - i2*ne1);

#ifdef LM_GGML_USE_ACCELERATE
            vDSP_vsub(
                    (float *) ((char *) src1->data + i3*nb13 + i2*nb12 + i1*nb11), 1,
                    (float *) ((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01), 1,
                    (float *) ((char *) dst->data  + i3*nb3  + i2*nb2  + i1*nb1 ), 1,
                    ne0);
#else
            lm_ggml_vec_sub_f32(ne0,
                    (float *) ((char *) dst->data  + i3*nb3  + i2*nb2  + i1*nb1 ),
                    (float *) ((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01),
                    (float *) ((char *) src1->data + i3*nb13 + i2*nb12 + i1*nb11));
#endif
                // }
            // }
        }
    } else {
        // src1 is not contiguous
        for (int ir = 0; ir < nr; ++ir) {
            // src0, src1 and dst are same shape => same indices
            const int i3 = ir/(ne2*ne1);
            const int i2 = (ir - i3*ne2*ne1)/ne1;
            const int i1 = (ir - i3*ne2*ne1 - i2*ne1);

            float * dst_ptr  = (float *) ((char *) dst->data  + i3*nb3  + i2*nb2  + i1*nb1 );
            float * src0_ptr = (float *) ((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01);
            for (int i0 = 0; i0 < ne0; i0++) {
                float * src1_ptr = (float *) ((char *) src1->data + i3*nb13 + i2*nb12 + i1*nb11 + i0*nb10);

                dst_ptr[i0] = src0_ptr[i0] - *src1_ptr;
            }
        }
    }
}

static void lm_ggml_compute_forward_sub(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_sub_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_mul

static void lm_ggml_compute_forward_mul_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_can_repeat_rows(src1, src0) && lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }
    const int ith = params->ith;
    const int nth = params->nth;

#ifdef LM_GGML_USE_CLBLAST
    if (src1->backend == LM_GGML_BACKEND_GPU) {
        if (ith == 0) {
            lm_ggml_cl_mul(src0, src1, dst);
        }
        return;
    }
#endif

    const int64_t nr = lm_ggml_nrows(src0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    LM_GGML_ASSERT( nb0 == sizeof(float));
    LM_GGML_ASSERT(nb00 == sizeof(float));
    LM_GGML_ASSERT(ne00 == ne10);

    if (nb10 == sizeof(float)) {
        for (int64_t ir = ith; ir < nr; ir += nth) {
            // src0 and dst are same shape => same indices
            const int64_t i03 = ir/(ne02*ne01);
            const int64_t i02 = (ir - i03*ne02*ne01)/ne01;
            const int64_t i01 = (ir - i03*ne02*ne01 - i02*ne01);

            const int64_t i13 = i03 % ne13;
            const int64_t i12 = i02 % ne12;
            const int64_t i11 = i01 % ne11;

            float * dst_ptr  = (float *) ((char *) dst->data  + i03*nb3  + i02*nb2  + i01*nb1 );
            float * src0_ptr = (float *) ((char *) src0->data + i03*nb03 + i02*nb02 + i01*nb01);
            float * src1_ptr = (float *) ((char *) src1->data + i13*nb13 + i12*nb12 + i11*nb11);

#ifdef LM_GGML_USE_ACCELERATE
            UNUSED(lm_ggml_vec_mul_f32);

            vDSP_vmul( src0_ptr, 1, src1_ptr, 1, dst_ptr,  1, ne00);
#else
            lm_ggml_vec_mul_f32(ne00, dst_ptr, src0_ptr, src1_ptr);
#endif
                // }
            // }
        }
    } else {
        // src1 is not contiguous
        for (int64_t ir = ith; ir < nr; ir += nth) {
            // src0 and dst are same shape => same indices
            // src1 is broadcastable across src0 and dst in i1, i2, i3
            const int64_t i03 = ir/(ne02*ne01);
            const int64_t i02 = (ir - i03*ne02*ne01)/ne01;
            const int64_t i01 = (ir - i03*ne02*ne01 - i02*ne01);

            const int64_t i13 = i03 % ne13;
            const int64_t i12 = i02 % ne12;
            const int64_t i11 = i01 % ne11;

            float * dst_ptr  = (float *) ((char *) dst->data  + i03*nb3  + i02*nb2  + i01*nb1 );
            float * src0_ptr = (float *) ((char *) src0->data + i03*nb03 + i02*nb02 + i01*nb01);

            for (int64_t i0 = 0; i0 < ne00; i0++) {
                float * src1_ptr = (float *) ((char *) src1->data + i13*nb13 + i12*nb12 + i11*nb11 + i0*nb10);

                dst_ptr[i0] = src0_ptr[i0] * (*src1_ptr);
            }
        }
    }
}

static void lm_ggml_compute_forward_mul(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F32 && "only f32 src1 supported for now");

    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_mul_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_div

static void lm_ggml_compute_forward_div_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);
    assert(lm_ggml_are_same_shape(src0, src1) && lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int nr  = lm_ggml_nrows(src0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    LM_GGML_ASSERT( nb0 == sizeof(float));
    LM_GGML_ASSERT(nb00 == sizeof(float));

    if (nb10 == sizeof(float)) {
        for (int ir = 0; ir < nr; ++ir) {
            // src0, src1 and dst are same shape => same indices
            const int i3 = ir/(ne2*ne1);
            const int i2 = (ir - i3*ne2*ne1)/ne1;
            const int i1 = (ir - i3*ne2*ne1 - i2*ne1);

#ifdef LM_GGML_USE_ACCELERATE
            UNUSED(lm_ggml_vec_div_f32);

            vDSP_vdiv(
                    (float *) ((char *) src1->data + i3*nb13 + i2*nb12 + i1*nb11), 1,
                    (float *) ((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01), 1,
                    (float *) ((char *) dst->data  + i3*nb3  + i2*nb2  + i1*nb1 ), 1,
                    ne0);
#else
            lm_ggml_vec_div_f32(ne0,
                    (float *) ((char *) dst->data  + i3*nb3  + i2*nb2  + i1*nb1 ),
                    (float *) ((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01),
                    (float *) ((char *) src1->data + i3*nb13 + i2*nb12 + i1*nb11));
#endif
                // }
            // }
        }
    } else {
        // src1 is not contiguous
        for (int ir = 0; ir < nr; ++ir) {
            // src0, src1 and dst are same shape => same indices
            const int i3 = ir/(ne2*ne1);
            const int i2 = (ir - i3*ne2*ne1)/ne1;
            const int i1 = (ir - i3*ne2*ne1 - i2*ne1);

            float * dst_ptr  = (float *) ((char *) dst->data  + i3*nb3  + i2*nb2  + i1*nb1 );
            float * src0_ptr = (float *) ((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01);
            for (int i0 = 0; i0 < ne0; i0++) {
                float * src1_ptr = (float *) ((char *) src1->data + i3*nb13 + i2*nb12 + i1*nb11 + i0*nb10);

                dst_ptr[i0] = src0_ptr[i0] / (*src1_ptr);
            }
        }
    }
}

static void lm_ggml_compute_forward_div(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_div_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_sqr

static void lm_ggml_compute_forward_sqr_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);
    assert(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int n     = lm_ggml_nrows(src0);
    const int nc    = src0->ne[0];

    assert( dst->nb[0] == sizeof(float));
    assert(src0->nb[0] == sizeof(float));

    for (int i = 0; i < n; i++) {
        lm_ggml_vec_sqr_f32(nc,
                (float *) ((char *) dst->data  + i*( dst->nb[1])),
                (float *) ((char *) src0->data + i*(src0->nb[1])));
    }
}

static void lm_ggml_compute_forward_sqr(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_sqr_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_sqrt

static void lm_ggml_compute_forward_sqrt_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);
    assert(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int n  = lm_ggml_nrows(src0);
    const int nc = src0->ne[0];

    assert( dst->nb[0] == sizeof(float));
    assert(src0->nb[0] == sizeof(float));

    for (int i = 0; i < n; i++) {
        lm_ggml_vec_sqrt_f32(nc,
                (float *) ((char *) dst->data  + i*( dst->nb[1])),
                (float *) ((char *) src0->data + i*(src0->nb[1])));
    }
}

static void lm_ggml_compute_forward_sqrt(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_sqrt_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_log

static void lm_ggml_compute_forward_log_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(params->ith == 0);
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int n  = lm_ggml_nrows(src0);
    const int nc = src0->ne[0];

    LM_GGML_ASSERT( dst->nb[0] == sizeof(float));
    LM_GGML_ASSERT(src0->nb[0] == sizeof(float));

    for (int i = 0; i < n; i++) {
        lm_ggml_vec_log_f32(nc,
                (float *) ((char *) dst->data  + i*( dst->nb[1])),
                (float *) ((char *) src0->data + i*(src0->nb[1])));
    }
}

static void lm_ggml_compute_forward_log(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_log_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_sum

static void lm_ggml_compute_forward_sum_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);
    assert(lm_ggml_is_scalar(dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    assert(lm_ggml_is_scalar(dst));
    assert(src0->nb[0] == sizeof(float));

    LM_GGML_TENSOR_LOCALS(int64_t, ne0, src0, ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nb0, src0, nb)

    lm_ggml_float sum     = 0;
    lm_ggml_float row_sum = 0;

    for (int64_t i03 = 0; i03 < ne03; i03++) {
        for (int64_t i02 = 0; i02 < ne02; i02++) {
            for (int64_t i01 = 0; i01 < ne01; i01++) {
                lm_ggml_vec_sum_f32_ggf(ne00,
                        &row_sum,
                        (float *) ((char *) src0->data + i01*nb01 + i02*nb02 + i03*nb03));
                sum += row_sum;
            }
        }
    }
    ((float *) dst->data)[0] = sum;
}

static void lm_ggml_compute_forward_sum_f16(
    const struct lm_ggml_compute_params * params,
    const struct lm_ggml_tensor * src0,
          struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);
    assert(lm_ggml_is_scalar(dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    assert(src0->nb[0] == sizeof(lm_ggml_fp16_t));

    LM_GGML_TENSOR_LOCALS(int64_t, ne0, src0, ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nb0, src0, nb)

    float sum = 0;
    float row_sum = 0;

    for (int64_t i03 = 0; i03 < ne03; i03++) {
        for (int64_t i02 = 0; i02 < ne02; i02++) {
            for (int64_t i01 = 0; i01 < ne01; i01++) {
                lm_ggml_vec_sum_f16_ggf(ne00,
                    &row_sum,
                    (lm_ggml_fp16_t *) ((char *) src0->data + i01 * nb01 + i02 * nb02 + i03 * nb03));
                sum += row_sum;
            }
        }
    }
    ((lm_ggml_fp16_t *) dst->data)[0] = LM_GGML_FP32_TO_FP16(sum);
}

static void lm_ggml_compute_forward_sum(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_sum_f32(params, src0, dst);
            } break;
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_sum_f16(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_sum_rows

static void lm_ggml_compute_forward_sum_rows_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_ASSERT(src0->nb[0] == sizeof(float));
    LM_GGML_ASSERT(dst->nb[0] == sizeof(float));

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    LM_GGML_ASSERT(ne0 == 1);
    LM_GGML_ASSERT(ne1 == ne01);
    LM_GGML_ASSERT(ne2 == ne02);
    LM_GGML_ASSERT(ne3 == ne03);

    for (int64_t i3 = 0; i3 < ne03; i3++) {
        for (int64_t i2 = 0; i2 < ne02; i2++) {
            for (int64_t i1 = 0; i1 < ne01; i1++) {
                float * src_row = (float *) ((char *) src0->data + i1*nb01 + i2*nb02 + i3*nb03);
                float * dst_row = (float *) ((char *) dst->data  + i1*nb1  + i2*nb2  + i3*nb3);
                float row_sum = 0;
                lm_ggml_vec_sum_f32(ne00, &row_sum, src_row);
                dst_row[0] = row_sum;
            }
        }
    }
}

static void lm_ggml_compute_forward_sum_rows(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_sum_rows_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_mean

static void lm_ggml_compute_forward_mean_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    assert(src0->nb[0] == sizeof(float));

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    assert(ne0 == 1);
    assert(ne1 == ne01);
    assert(ne2 == ne02);
    assert(ne3 == ne03);

    UNUSED(ne0);
    UNUSED(ne1);
    UNUSED(ne2);
    UNUSED(ne3);

    for (int64_t i03 = 0; i03 < ne03; i03++) {
        for (int64_t i02 = 0; i02 < ne02; i02++) {
            for (int64_t i01 = 0; i01 < ne01; i01++) {
                lm_ggml_vec_sum_f32(ne00,
                        (float *) ((char *)  dst->data + i01*nb1  + i02*nb2  + i03*nb3),
                        (float *) ((char *) src0->data + i01*nb01 + i02*nb02 + i03*nb03));

                *(float *) ((char *) dst->data + i01*nb1 + i02*nb2 + i03*nb3) /= (float) ne00;
            }
        }
    }
}

static void lm_ggml_compute_forward_mean(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_mean_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_argmax

static void lm_ggml_compute_forward_argmax_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    assert(src0->nb[0] == sizeof(float));
    assert(dst->nb[0] == sizeof(float));

    const int64_t ne00 = src0->ne[0];
    const int64_t ne01 = src0->ne[1];

    const size_t nb01 = src0->nb[1];
    const size_t nb0 = dst->nb[0];

    for (int64_t i1 = 0; i1 < ne01; i1++) {
        float * src = (float *) ((char *) src0->data + i1*nb01);
        int32_t * dst_ = (int32_t *) ((char *)  dst->data + i1*nb0);
        int v = 0;
        lm_ggml_vec_argmax_f32(ne00, &v, src);
        dst_[0] = v;
    }
}

static void lm_ggml_compute_forward_argmax(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_argmax_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_repeat

static void lm_ggml_compute_forward_repeat_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(params->ith == 0);
    LM_GGML_ASSERT(lm_ggml_can_repeat(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    // guaranteed to be an integer due to the check in lm_ggml_can_repeat
    const int nr0 = (int)(ne0/ne00);
    const int nr1 = (int)(ne1/ne01);
    const int nr2 = (int)(ne2/ne02);
    const int nr3 = (int)(ne3/ne03);

    // TODO: support for transposed / permuted tensors
    LM_GGML_ASSERT(nb0  == sizeof(float));
    LM_GGML_ASSERT(nb00 == sizeof(float));

    // TODO: maybe this is not optimal?
    for                         (int i3 = 0; i3 < nr3;  i3++) {
        for                     (int k3 = 0; k3 < ne03; k3++) {
            for                 (int i2 = 0; i2 < nr2;  i2++) {
                for             (int k2 = 0; k2 < ne02; k2++) {
                    for         (int i1 = 0; i1 < nr1;  i1++) {
                        for     (int k1 = 0; k1 < ne01; k1++) {
                            for (int i0 = 0; i0 < nr0;  i0++) {
                                lm_ggml_vec_cpy_f32(ne00,
                                        (float *) ((char *)  dst->data + (i3*ne03 + k3)*nb3  + (i2*ne02 + k2)*nb2  + (i1*ne01 + k1)*nb1  + (i0*ne00)*nb0),
                                        (float *) ((char *) src0->data + (          k3)*nb03 + (          k2)*nb02 + (          k1)*nb01));
                            }
                        }
                    }
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_repeat_f16(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(params->ith == 0);
    LM_GGML_ASSERT(lm_ggml_can_repeat(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_TENSOR_UNARY_OP_LOCALS;

    // guaranteed to be an integer due to the check in lm_ggml_can_repeat
    const int nr0 = (int)(ne0/ne00);
    const int nr1 = (int)(ne1/ne01);
    const int nr2 = (int)(ne2/ne02);
    const int nr3 = (int)(ne3/ne03);

    // TODO: support for transposed / permuted tensors
    LM_GGML_ASSERT(nb0  == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb00 == sizeof(lm_ggml_fp16_t));

    // TODO: maybe this is not optimal?
    for                         (int i3 = 0; i3 < nr3;  i3++) {
        for                     (int k3 = 0; k3 < ne03; k3++) {
            for                 (int i2 = 0; i2 < nr2;  i2++) {
                for             (int k2 = 0; k2 < ne02; k2++) {
                    for         (int i1 = 0; i1 < nr1;  i1++) {
                        for     (int k1 = 0; k1 < ne01; k1++) {
                            for (int i0 = 0; i0 < nr0;  i0++) {
                                lm_ggml_fp16_t * y = (lm_ggml_fp16_t *) ((char *)  dst->data + (i3*ne03 + k3)*nb3  + (i2*ne02 + k2)*nb2  + (i1*ne01 + k1)*nb1  + (i0*ne00)*nb0);
                                lm_ggml_fp16_t * x = (lm_ggml_fp16_t *) ((char *) src0->data + (          k3)*nb03 + (          k2)*nb02 + (          k1)*nb01);
                                // lm_ggml_vec_cpy_f16(ne00, y, x)
                                for (int i = 0; i < ne00; ++i) {
                                    y[i]  = x[i];
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_repeat(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_repeat_f16(params, src0, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_repeat_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_repeat_back

static void lm_ggml_compute_forward_repeat_back_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(params->ith == 0);
    LM_GGML_ASSERT(lm_ggml_can_repeat(dst, src0));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    // guaranteed to be an integer due to the check in lm_ggml_can_repeat
    const int nr0 = (int)(ne00/ne0);
    const int nr1 = (int)(ne01/ne1);
    const int nr2 = (int)(ne02/ne2);
    const int nr3 = (int)(ne03/ne3);

    // TODO: support for transposed / permuted tensors
    LM_GGML_ASSERT(nb0  == sizeof(float));
    LM_GGML_ASSERT(nb00 == sizeof(float));

    if (lm_ggml_is_contiguous(dst)) {
        lm_ggml_vec_set_f32(ne0*ne1*ne2*ne3, dst->data, 0);
    } else {
        for         (int k3 = 0; k3 < ne3; k3++) {
            for     (int k2 = 0; k2 < ne2; k2++) {
                for (int k1 = 0; k1 < ne1; k1++) {
                    lm_ggml_vec_set_f32(ne0,
                        (float *) ((char *) dst->data + k1*nb1 + k2*nb2 + k3*nb3),
                        0);
                }
            }
        }
    }

    // TODO: maybe this is not optimal?
    for                         (int i3 = 0; i3 < nr3; i3++) {
        for                     (int k3 = 0; k3 < ne3; k3++) {
            for                 (int i2 = 0; i2 < nr2; i2++) {
                for             (int k2 = 0; k2 < ne2; k2++) {
                    for         (int i1 = 0; i1 < nr1; i1++) {
                        for     (int k1 = 0; k1 < ne1; k1++) {
                            for (int i0 = 0; i0 < nr0; i0++) {
                                lm_ggml_vec_acc_f32(ne0,
                                        (float *) ((char *)  dst->data + (         k3)*nb3  + (         k2)*nb2  + (         k1)*nb1),
                                        (float *) ((char *) src0->data + (i3*ne3 + k3)*nb03 + (i2*ne2 + k2)*nb02 + (i1*ne1 + k1)*nb01 + (i0*ne0)*nb00));
                            }
                        }
                    }
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_repeat_back(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_repeat_back_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_concat

static void lm_ggml_compute_forward_concat_f32(
    const struct lm_ggml_compute_params * params,
    const struct lm_ggml_tensor * src0,
    const struct lm_ggml_tensor * src1,
    struct lm_ggml_tensor * dst) {

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_ASSERT(src0->nb[0] == sizeof(float));

    const int ith = params->ith;

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    // TODO: support for transposed / permuted tensors
    LM_GGML_ASSERT(nb0  == sizeof(float));
    LM_GGML_ASSERT(nb00 == sizeof(float));
    LM_GGML_ASSERT(nb10 == sizeof(float));

    for (int i3 = 0; i3 < ne3; i3++) {
        for (int i2 = ith; i2 < ne2; i2++) {
            if (i2 < ne02) { // src0
                for (int i1 = 0; i1 < ne1; i1++) {
                    for (int i0 = 0; i0 < ne0; i0++) {
                        const float * x = (float *)((char *) src0->data + i0 * nb00 + i1 * nb01 + i2 * nb02 + i3 * nb03);

                        float * y = (float *)((char *)dst->data + i0 * nb0 + i1 * nb1 + i2 * nb2 + i3 * nb3);
                        *y = *x;
                    }
                }
            } // src1
            else {
                for (int i1 = 0; i1 < ne1; i1++) {
                    for (int i0 = 0; i0 < ne0; i0++) {
                        const float * x = (float *)((char *) src1->data + i0 * nb10 + i1 * nb11 + (i2 - ne02) * nb12 + i3 * nb13);

                        float * y = (float *)((char *)dst->data + i0 * nb0 + i1 * nb1 + i2 * nb2 + i3 * nb3);
                        *y = *x;
                    }
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_concat(
    const struct lm_ggml_compute_params* params,
    const struct lm_ggml_tensor* src0,
    const struct lm_ggml_tensor* src1,
    struct lm_ggml_tensor* dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_concat_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_abs

static void lm_ggml_compute_forward_abs_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);
    assert(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int n  = lm_ggml_nrows(src0);
    const int nc = src0->ne[0];

    assert(dst->nb[0]  == sizeof(float));
    assert(src0->nb[0] == sizeof(float));

    for (int i = 0; i < n; i++) {
        lm_ggml_vec_abs_f32(nc,
                (float *) ((char *) dst->data  + i*( dst->nb[1])),
                (float *) ((char *) src0->data + i*(src0->nb[1])));
    }
}

static void lm_ggml_compute_forward_abs(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_abs_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_sgn

static void lm_ggml_compute_forward_sgn_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);
    assert(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int n  = lm_ggml_nrows(src0);
    const int nc = src0->ne[0];

    assert(dst->nb[0]  == sizeof(float));
    assert(src0->nb[0] == sizeof(float));

    for (int i = 0; i < n; i++) {
        lm_ggml_vec_sgn_f32(nc,
                (float *) ((char *) dst->data  + i*( dst->nb[1])),
                (float *) ((char *) src0->data + i*(src0->nb[1])));
    }
}

static void lm_ggml_compute_forward_sgn(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_sgn_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_neg

static void lm_ggml_compute_forward_neg_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);
    assert(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int n  = lm_ggml_nrows(src0);
    const int nc = src0->ne[0];

    assert(dst->nb[0]  == sizeof(float));
    assert(src0->nb[0] == sizeof(float));

    for (int i = 0; i < n; i++) {
        lm_ggml_vec_neg_f32(nc,
                (float *) ((char *) dst->data  + i*( dst->nb[1])),
                (float *) ((char *) src0->data + i*(src0->nb[1])));
    }
}

static void lm_ggml_compute_forward_neg(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_neg_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_step

static void lm_ggml_compute_forward_step_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);
    assert(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int n  = lm_ggml_nrows(src0);
    const int nc = src0->ne[0];

    assert(dst->nb[0]  == sizeof(float));
    assert(src0->nb[0] == sizeof(float));

    for (int i = 0; i < n; i++) {
        lm_ggml_vec_step_f32(nc,
                (float *) ((char *) dst->data  + i*( dst->nb[1])),
                (float *) ((char *) src0->data + i*(src0->nb[1])));
    }
}

static void lm_ggml_compute_forward_step(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_step_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_tanh

static void lm_ggml_compute_forward_tanh_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);
    assert(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int n  = lm_ggml_nrows(src0);
    const int nc = src0->ne[0];

    assert(dst->nb[0]  == sizeof(float));
    assert(src0->nb[0] == sizeof(float));

    for (int i = 0; i < n; i++) {
        lm_ggml_vec_tanh_f32(nc,
                (float *) ((char *) dst->data  + i*( dst->nb[1])),
                (float *) ((char *) src0->data + i*(src0->nb[1])));
    }
}

static void lm_ggml_compute_forward_tanh(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_tanh_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_elu

static void lm_ggml_compute_forward_elu_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);
    assert(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int n  = lm_ggml_nrows(src0);
    const int nc = src0->ne[0];

    assert(dst->nb[0]  == sizeof(float));
    assert(src0->nb[0] == sizeof(float));

    for (int i = 0; i < n; i++) {
        lm_ggml_vec_elu_f32(nc,
                (float *) ((char *) dst->data  + i*( dst->nb[1])),
                (float *) ((char *) src0->data + i*(src0->nb[1])));
    }
}

static void lm_ggml_compute_forward_elu(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_elu_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_relu

static void lm_ggml_compute_forward_relu_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);
    assert(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int n  = lm_ggml_nrows(src0);
    const int nc = src0->ne[0];

    assert(dst->nb[0]  == sizeof(float));
    assert(src0->nb[0] == sizeof(float));

    for (int i = 0; i < n; i++) {
        lm_ggml_vec_relu_f32(nc,
                (float *) ((char *) dst->data  + i*( dst->nb[1])),
                (float *) ((char *) src0->data + i*(src0->nb[1])));
    }
}

static void lm_ggml_compute_forward_relu(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_relu_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_gelu

static void lm_ggml_compute_forward_gelu_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_is_contiguous_except_dim_1(src0));
    LM_GGML_ASSERT(lm_ggml_is_contiguous_except_dim_1(dst));
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int ith = params->ith;
    const int nth = params->nth;

    const int nc = src0->ne[0];
    const int nr = lm_ggml_nrows(src0);

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    for (int i1 = ir0; i1 < ir1; i1++) {
        lm_ggml_vec_gelu_f32(nc,
                (float *) ((char *) dst->data  + i1*( dst->nb[1])),
                (float *) ((char *) src0->data + i1*(src0->nb[1])));

#ifndef NDEBUG
        for (int k = 0; k < nc; k++) {
            const float x = ((float *) ((char *) dst->data + i1*( dst->nb[1])))[k];
            UNUSED(x);
            assert(!isnan(x));
            assert(!isinf(x));
        }
#endif
    }
}

static void lm_ggml_compute_forward_gelu(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_gelu_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_gelu_quick

static void lm_ggml_compute_forward_gelu_quick_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_is_contiguous_except_dim_1(src0));
    LM_GGML_ASSERT(lm_ggml_is_contiguous_except_dim_1(dst));
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int ith = params->ith;
    const int nth = params->nth;

    const int nc = src0->ne[0];
    const int nr = lm_ggml_nrows(src0);

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    for (int i1 = ir0; i1 < ir1; i1++) {
        lm_ggml_vec_gelu_quick_f32(nc,
                (float *) ((char *) dst->data  + i1*( dst->nb[1])),
                (float *) ((char *) src0->data + i1*(src0->nb[1])));

#ifndef NDEBUG
        for (int k = 0; k < nc; k++) {
            const float x = ((float *) ((char *) dst->data + i1*( dst->nb[1])))[k];
            UNUSED(x);
            assert(!isnan(x));
            assert(!isinf(x));
        }
#endif
    }
}

static void lm_ggml_compute_forward_gelu_quick(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_gelu_quick_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_silu

static void lm_ggml_compute_forward_silu_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_is_contiguous_except_dim_1(src0));
    LM_GGML_ASSERT(lm_ggml_is_contiguous_except_dim_1(dst));
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int ith = params->ith;
    const int nth = params->nth;

    const int nc = src0->ne[0];
    const int nr = lm_ggml_nrows(src0);

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    for (int i1 = ir0; i1 < ir1; i1++) {
        lm_ggml_vec_silu_f32(nc,
                (float *) ((char *) dst->data  + i1*( dst->nb[1])),
                (float *) ((char *) src0->data + i1*(src0->nb[1])));

#ifndef NDEBUG
        for (int k = 0; k < nc; k++) {
            const float x = ((float *) ((char *) dst->data + i1*(dst->nb[1])))[k];
            UNUSED(x);
            assert(!isnan(x));
            assert(!isinf(x));
        }
#endif
    }
}

static void lm_ggml_compute_forward_silu(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_silu_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_silu_back

static void lm_ggml_compute_forward_silu_back_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * grad,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_is_contiguous_except_dim_1(grad));
    LM_GGML_ASSERT(lm_ggml_is_contiguous_except_dim_1(src0));
    LM_GGML_ASSERT(lm_ggml_is_contiguous_except_dim_1(dst));
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, grad));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int ith = params->ith;
    const int nth = params->nth;

    const int nc = src0->ne[0];
    const int nr = lm_ggml_nrows(src0);

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    for (int i1 = ir0; i1 < ir1; i1++) {
        lm_ggml_vec_silu_backward_f32(nc,
                (float *) ((char *) dst->data  + i1*( dst->nb[1])),
                (float *) ((char *) src0->data + i1*(src0->nb[1])),
                (float *) ((char *) grad->data + i1*(grad->nb[1])));

#ifndef NDEBUG
        for (int k = 0; k < nc; k++) {
            const float x = ((float *) ((char *) dst->data + i1*( dst->nb[1])))[k];
            UNUSED(x);
            assert(!isnan(x));
            assert(!isinf(x));
        }
#endif
    }
}

static void lm_ggml_compute_forward_silu_back(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * grad,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_silu_back_f32(params, src0, grad, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_norm

static void lm_ggml_compute_forward_norm_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_ASSERT(src0->nb[0] == sizeof(float));

    const int ith = params->ith;
    const int nth = params->nth;

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    float eps;
    memcpy(&eps, dst->op_params, sizeof(float));

    // TODO: optimize
    for (int64_t i03 = 0; i03 < ne03; i03++) {
        for (int64_t i02 = 0; i02 < ne02; i02++) {
            for (int64_t i01 = ith; i01 < ne01; i01 += nth) {
                const float * x = (float *) ((char *) src0->data + i01*nb01 + i02*nb02 + i03*nb03);

                lm_ggml_float sum = 0.0;
                for (int64_t i00 = 0; i00 < ne00; i00++) {
                    sum += (lm_ggml_float)x[i00];
                }

                float mean = sum/ne00;

                float * y = (float *) ((char *) dst->data + i01*nb1 + i02*nb2 + i03*nb3);

                lm_ggml_float sum2 = 0.0;
                for (int64_t i00 = 0; i00 < ne00; i00++) {
                    float v = x[i00] - mean;
                    y[i00] = v;
                    sum2 += (lm_ggml_float)(v*v);
                }

                float variance = sum2/ne00;
                const float scale = 1.0f/sqrtf(variance + eps);

                lm_ggml_vec_scale_f32(ne00, y, scale);
            }
        }
    }
}

static void lm_ggml_compute_forward_norm(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_norm_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_group_rms_norm

static void lm_ggml_compute_forward_rms_norm_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_ASSERT(src0->nb[0] == sizeof(float));

    const int ith = params->ith;
    const int nth = params->nth;

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    float eps;
    memcpy(&eps, dst->op_params, sizeof(float));

    // TODO: optimize
    for (int64_t i03 = 0; i03 < ne03; i03++) {
        for (int64_t i02 = 0; i02 < ne02; i02++) {
            for (int64_t i01 = ith; i01 < ne01; i01 += nth) {
                const float * x = (float *) ((char *) src0->data + i01*nb01 + i02*nb02 + i03*nb03);

                lm_ggml_float sum = 0.0;
                for (int64_t i00 = 0; i00 < ne00; i00++) {
                    sum += (lm_ggml_float)(x[i00] * x[i00]);
                }

                const float mean = sum/ne00;

                float * y = (float *) ((char *) dst->data + i01*nb1 + i02*nb2 + i03*nb3);

                memcpy(y, x, ne00 * sizeof(float));
                // for (int i00 = 0; i00 < ne00; i00++) {
                //     y[i00] = x[i00];
                // }

                const float scale = 1.0f/sqrtf(mean + eps);

                lm_ggml_vec_scale_f32(ne00, y, scale);
            }
        }
    }
}

static void lm_ggml_compute_forward_rms_norm(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_rms_norm_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

static void lm_ggml_compute_forward_rms_norm_back_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst) && lm_ggml_are_same_shape(src0, src1));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_ASSERT(src0->nb[0] == sizeof(float));

    const int ith = params->ith;
    const int nth = params->nth;

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    float eps;
    memcpy(&eps, dst->op_params, sizeof(float));

    // TODO: optimize
    for (int64_t i03 = 0; i03 < ne03; i03++) {
        for (int64_t i02 = 0; i02 < ne02; i02++) {
            for (int64_t i01 = ith; i01 < ne01; i01 += nth) {
                // src1 is same shape as src0 => same indices
                const int64_t i11 = i01;
                const int64_t i12 = i02;
                const int64_t i13 = i03;

                const float * x = (float *) ((char *) src0->data + i01*nb01 + i02*nb02 + i03*nb03);
                const float * dz = (float *) ((char *) src1->data + i11*nb11 + i12*nb12 + i13*nb13);

                lm_ggml_float sum_xx  = 0.0;
                lm_ggml_float sum_xdz = 0.0;

                for (int64_t i00 = 0; i00 < ne00; i00++) {
                    sum_xx  += (lm_ggml_float)(x[i00] * x[i00]);
                    sum_xdz += (lm_ggml_float)(x[i00] * dz[i00]);
                }

                //const float mean     = (float)(sum_xx)/ne00;
                const float mean_eps = (float)(sum_xx)/ne00 + eps;
                const float sum_eps  = (float)(sum_xx) + eps*ne00;
                //const float mean_xdz = (float)(sum_xdz)/ne00;
                // we could cache rms from forward pass to improve performance.
                // to do this implement lm_ggml_rms and compose lm_ggml_rms_norm using lm_ggml_rms.
                //const float rms      = sqrtf(mean_eps);
                const float rrms     = 1.0f / sqrtf(mean_eps);
                //const float scale    = -rrms/(ne00 * mean_eps); // -1/(n*rms**3)

                {
                    // z = rms_norm(x)
                    //
                    // rms_norm(src0) =
                    //     scale(
                    //         src0,
                    //         div(
                    //             1,
                    //             sqrt(
                    //                 add(
                    //                     scale(
                    //                         sum(
                    //                             sqr(
                    //                                 src0)),
                    //                         (1.0/N)),
                    //                     eps))));

                    // postorder:
                    // ## op    args         grad
                    // 00 param src0         grad[#00]
                    // 01 const 1
                    // 02 sqr   (#00)        grad[#02]
                    // 03 sum   (#02)        grad[#03]
                    // 04 const 1/N
                    // 05 scale (#03, #04)   grad[#05]
                    // 06 const eps
                    // 07 add   (#05, #06)   grad[#07]
                    // 08 sqrt  (#07)        grad[#08]
                    // 09 div   (#01,#08)    grad[#09]
                    // 10 scale (#00,#09)    grad[#10]
                    //
                    // backward pass, given grad[#10]
                    // #10: scale
                    // grad[#00] += scale(grad[#10],#09)
                    // grad[#09] += sum(mul(grad[#10],#00))
                    // #09: div
                    // grad[#08] += neg(mul(grad[#09], div(#09,#08)))
                    // #08: sqrt
                    // grad[#07] += mul(grad[#08], div(0.5, #08))
                    // #07: add
                    // grad[#05] += grad[#07]
                    // #05: scale
                    // grad[#03] += scale(grad[#05],#04)
                    // #03: sum
                    // grad[#02] += repeat(grad[#03], #02)
                    // #02:
                    // grad[#00] += scale(mul(#00, grad[#02]), 2.0)
                    //
                    // substitute and simplify:
                    // grad[#00] = scale(grad(#10), #09) + scale(mul(#00, grad[#02]), 2.0)
                    // grad[#02] = repeat(grad[#03], #02)
                    // grad[#02] = repeat(scale(grad[#05],#04), #02)
                    // grad[#02] = repeat(scale(grad[#07],#04), #02)
                    // grad[#02] = repeat(scale(mul(grad[#08], div(0.5, #08)),#04), #02)
                    // grad[#02] = repeat(scale(mul(neg(mul(grad[#09], div(#09,#08))), div(0.5, #08)),#04), #02)
                    // grad[#02] = repeat(scale(mul(neg(mul(sum(mul(grad[#10],#00)), div(#09,#08))), div(0.5, #08)),#04), #02)
                    // grad[#02] = repeat(-(sum(mul(grad[#10],#00)) * div(#09,#08) * div(0.5, #08) * (1/N)), #02)
                    // grad[#02] = repeat(-(sum(mul(grad[#10],#00)) * div(div(#01,#08),#08) * div(0.5, #08) * (1/N)), #02)
                    // grad[#02] = repeat(-(sum(mul(grad[#10],#00)) * div(1,#08*#08) * div(0.5, #08) * (1/N)), #02)
                    // grad[#02] = repeat(-(sum(mul(grad[#10],#00)) * div(1,#07) * div(0.5, #08) * (1/N)), #02)
                    // grad[#00] = scale(grad(#10), #09) + scale(mul(#00, grad[#02]), 2.0)
                    // grad[#00] = scale(grad(#10), #09) + scale(mul(#00, repeat(-(sum(mul(grad[#10],#00)) * div(1,#07) * div(0.5, #08) * (1/N)), #02)), 2.0)
                    // grad[#00] = scale(grad(#10), #09) + scale(scale(#00, -(sum(mul(grad[#10],#00)) * div(1,#07) * div(0.5, #08) * (1/N))), 2.0)
                    // grad[#00] = scale(grad(#10), #09) + scale(#00, -(sum(mul(grad[#10],#00)) * div(1,#07) * div(1,#08) * (1/N)))
                    // grad[#00] = scale(grad(#10), #09) + scale(#00, sum(mul(grad[#10],#00)) * div(1,#07*#08) * (-1/N))
                    // grad[#00] = scale(grad(#10), #09) + scale(#00, sum(mul(grad[#10],#00)) * div(1,#07*#08) * (-1/N))
                    // grad[#00] = scale(grad(#10), #09) + scale(#00, sum(mul(grad[#10],#00)) * div(1,mean_eps*rms) * (-1/N))
                    // grad[#00] = scale(grad(#10), #09) + scale(#00, sum(mul(grad[#10],#00)) * div(-1,rms*N*mean_eps))
                    // grad[#00] = scale(grad(#10), #09) + scale(#00, sum(mul(grad[#10],#00)) * div(-1,rms*N*(sum_xx/N+eps)))
                    // grad[#00] = scale(grad(#10), #09) + scale(#00, sum(mul(grad[#10],#00)) * div(-1,rms*N*sum_xx+rms*N*eps))
                    // grad[#00] = scale(dz, rrms) + scale(x, sum(mul(dz,x)) * div(-1,rms*N*mean_eps))
                    // grad[#00] = scale(dz, rrms) + scale(x, sum_xdz * div(-1,rms*N*mean_eps))
                    // a = b*c + d*e
                    // a = b*c*f/f + d*e*f/f
                    // a = (b*c*f + d*e*f)*(1/f)
                    // a = (b*c*(1/c) + d*e*(1/c))*(1/(1/c))
                    // a = (b + d*e/c)*c
                    // b = dz, c = rrms, d = x, e = sum_xdz * div(-1,rms*N*mean_eps)
                    // a = (dz + x*sum_xdz * div(-1,rms*N*mean_eps)/rrms)*rrms
                    // a = (dz + x*sum_xdz * div(-1,rms*N*mean_eps)*rms)*rrms
                    // a = (dz + x*sum_xdz * div(-rms,rms*N*mean_eps))*rrms
                    // a = (dz + x*sum_xdz * div(-1,N*mean_eps))*rrms
                    // a = (dz + x*div(-sum_xdz,N*mean_eps))*rrms
                    // a = (dz + x*div(-mean_xdz,mean_eps))*rrms
                    // grad[#00] = scale(dz + scale(x, div(-mean_xdz,mean_eps)),rrms)
                    // grad[#00] = scale(dz + scale(x, -mean_xdz/mean_eps),rrms)
                    // dx = scale(dz + scale(x, -mean_xdz/mean_eps),rrms)
                }
                // dx = scale(dz + scale(x, -mean_xdz/mean_eps),rrms)
                // post-order:
                // dx := x
                // dx := scale(dx,-mean_xdz/mean_eps)
                // dx := add(dx, dz)
                // dx := scale(dx, rrms)
                float * dx = (float *) ((char *) dst->data + i01*nb1 + i02*nb2 + i03*nb3);

                lm_ggml_vec_cpy_f32  (ne00, dx, x);
                // lm_ggml_vec_scale_f32(ne00, dx, -mean_xdz/mean_eps);
                lm_ggml_vec_scale_f32(ne00, dx, (float)(-sum_xdz)/sum_eps);
                lm_ggml_vec_acc_f32  (ne00, dx, dz);
                lm_ggml_vec_scale_f32(ne00, dx, rrms);
            }
        }
    }
}

static void lm_ggml_compute_forward_rms_norm_back(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_rms_norm_back_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_group_norm

static void lm_ggml_compute_forward_group_norm_f32(
    const struct lm_ggml_compute_params * params,
    const struct lm_ggml_tensor * src0,
    struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_ASSERT(src0->nb[0] == sizeof(float));

    const int ith = params->ith;
    const int nth = params->nth;

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    const float eps = 1e-6f; // TODO: make this a parameter

    // TODO: optimize

    int n_channels = src0->ne[2];
    int n_groups = dst->op_params[0];
    int n_channels_per_group = (n_channels + n_groups - 1) / n_groups;
    for (int i = ith; i < n_groups; i+=nth) {
        int start = i * n_channels_per_group;
        int end = start + n_channels_per_group;
        if (end > n_channels) {
            end = n_channels;
        }
        int step = end - start;

        for (int64_t i03 = 0; i03 < ne03; i03++) {
            lm_ggml_float sum = 0.0;
            for (int64_t i02 = start; i02 < end; i02++) {
                for (int64_t i01 = 0; i01 < ne01; i01++) {
                    const float * x = (float *)((char *) src0->data + i01 * nb01 + i02 * nb02 + i03 * nb03);

                    for (int64_t i00 = 0; i00 < ne00; i00++) {
                        sum += (lm_ggml_float)x[i00];
                    }
                }
            }
            float mean = sum / (ne00 * ne01 * step);
            lm_ggml_float sum2 = 0.0;

            for (int64_t i02 = start; i02 < end; i02++) {
                for (int64_t i01 = 0; i01 < ne01; i01++) {
                    const float * x = (float *)((char *) src0->data + i01 * nb01 + i02 * nb02 + i03 * nb03);

                    float * y = (float *)((char *) dst->data + i01 * nb1 + i02 * nb2 + i03 * nb3);

                    for (int64_t i00 = 0; i00 < ne00; i00++) {
                        float v = x[i00] - mean;
                        y[i00] = v;
                        sum2 += (lm_ggml_float)(v * v);
                    }
                }
            }
            float variance = sum2 / (ne00 * ne01 * step);
            const float scale = 1.0f / sqrtf(variance + eps);

            for (int64_t i02 = start; i02 < end; i02++) {
                for (int64_t i01 = 0; i01 < ne01; i01++) {
                    float * y = (float *)((char *) dst->data + i01 * nb1 + i02 * nb2 + i03 * nb3);
                    lm_ggml_vec_scale_f32(ne00, y, scale);
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_group_norm(
    const struct lm_ggml_compute_params * params,
    const struct lm_ggml_tensor * src0,
    struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_group_norm_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_mul_mat

#if defined(LM_GGML_USE_ACCELERATE) || defined(LM_GGML_USE_OPENBLAS)
// helper function to determine if it is better to use BLAS or not
// for large matrices, BLAS is faster
static bool lm_ggml_compute_forward_mul_mat_use_blas(
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    //const int64_t ne00 = src0->ne[0];
    //const int64_t ne01 = src0->ne[1];

    const int64_t ne10 = src1->ne[0];

    const int64_t ne0 = dst->ne[0];
    const int64_t ne1 = dst->ne[1];

    // TODO: find the optimal values for these
    if (lm_ggml_is_contiguous(src0) &&
        lm_ggml_is_contiguous(src1) &&
        (ne0 >= 32 && ne1 >= 32 && ne10 >= 32)) {

        /*printf("BLAS: %d %d %d %d %d\n", ne0, ne1, ne10, ne00, ne01);*/
        return true;
    }

    return false;
}
#endif

static void lm_ggml_compute_forward_mul_mat(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    const int ith = params->ith;
    const int nth = params->nth;

    const enum lm_ggml_type type = src0->type;

    const bool src1_cont = lm_ggml_is_contiguous(src1);

    lm_ggml_vec_dot_t    const vec_dot               = type_traits[type].vec_dot;
    enum lm_ggml_type    const vec_dot_type          = type_traits[type].vec_dot_type;
    lm_ggml_from_float_t const from_float_to_vec_dot = type_traits[vec_dot_type].from_float;

    LM_GGML_ASSERT(ne0 == ne01);
    LM_GGML_ASSERT(ne1 == ne11);
    LM_GGML_ASSERT(ne2 == ne12);
    LM_GGML_ASSERT(ne3 == ne13);

    // we don't support permuted src0 or src1
    LM_GGML_ASSERT(nb00 == lm_ggml_type_size(type));
    LM_GGML_ASSERT(nb10 == sizeof(float));

    // dst cannot be transposed or permuted
    LM_GGML_ASSERT(nb0 == sizeof(float));
    LM_GGML_ASSERT(nb0 <= nb1);
    LM_GGML_ASSERT(nb1 <= nb2);
    LM_GGML_ASSERT(nb2 <= nb3);

    // broadcast factors
    const int64_t r2 = ne12/ne02;
    const int64_t r3 = ne13/ne03;

    // nb01 >= nb00 - src0 is not transposed
    //   compute by src0 rows

#if defined(LM_GGML_USE_CLBLAST)
    if (lm_ggml_cl_can_mul_mat(src0, src1, dst)) {
        if (params->ith == 0 && params->type == LM_GGML_TASK_COMPUTE) {
            lm_ggml_cl_mul_mat(src0, src1, dst, params->wdata, params->wsize);
        }
        return;
    }
#endif

#if defined(LM_GGML_USE_ACCELERATE) || defined(LM_GGML_USE_OPENBLAS)
    if (lm_ggml_compute_forward_mul_mat_use_blas(src0, src1, dst)) {
        if (params->ith != 0) {
            return;
        }

        if (params->type == LM_GGML_TASK_INIT) {
            return;
        }

        if (params->type == LM_GGML_TASK_FINALIZE) {
            return;
        }

        for (int64_t i13 = 0; i13 < ne13; i13++) {
            for (int64_t i12 = 0; i12 < ne12; i12++) {
                // broadcast src0 into src1 across 2nd,3rd dimension
                const int64_t i03 = i13/r3;
                const int64_t i02 = i12/r2;

                const void  * x = (char *)            src0->data + i02*nb02 + i03*nb03;
                const float * y = (float *) ((char *) src1->data + i12*nb12 + i13*nb13);

                float * d = (float *) ((char *) dst->data + i12*nb2 + i13*nb3);

                if (type != LM_GGML_TYPE_F32) {
                            float * const wdata    = params->wdata;
                    lm_ggml_to_float_t const to_float = type_traits[type].to_float;

                    size_t id = 0;
                    for (int64_t i01 = 0; i01 < ne01; ++i01) {
                        to_float((const char *) x + i01*nb01, wdata + id, ne00);
                        id += ne00;
                    }

                    assert(id*sizeof(float) <= params->wsize);
                    x = wdata;
                }

                cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                        ne11, ne01, ne10,
                        1.0f,    y, ne10,
                                 x, ne00,
                        0.0f,    d, ne01);
            }
        }

        //printf("CBLAS = %f ms, %d x %d x %d x %d\n", (lm_ggml_perf_time_us() - t0)/1000.0, ne0, ne1, ne2, ne3);

        return;
    }
#endif

    if (params->type == LM_GGML_TASK_INIT) {
        if (src1->type != vec_dot_type) {
            char * wdata = params->wdata;
            const size_t row_size = ne10*lm_ggml_type_size(vec_dot_type)/lm_ggml_blck_size(vec_dot_type);

            for (int64_t i13 = 0; i13 < ne13; ++i13) {
                for (int64_t i12 = 0; i12 < ne12; ++i12) {
                    for (int64_t i11 = 0; i11 < ne11; ++i11) {
                        from_float_to_vec_dot((float *)((char *) src1->data + i13*nb13 + i12*nb12 + i11*nb11), (void *) wdata, ne10);
                        wdata += row_size;
                    }
                }
            }
        }

        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const void * wdata    = (src1->type == vec_dot_type) ? src1->data : params->wdata;
    const size_t row_size = ne10*lm_ggml_type_size(vec_dot_type)/lm_ggml_blck_size(vec_dot_type);

    const int64_t nr0 = ne01;           // src0 rows
    const int64_t nr1 = ne11*ne12*ne13; // src1 rows

    //printf("nr0 = %lld, nr1 = %lld\n", nr0, nr1);

    // distribute the thread work across the inner or outer loop based on which one is larger

    const int64_t nth0 = nr0 > nr1 ? nth : 1; // parallelize by src0 rows
    const int64_t nth1 = nr0 > nr1 ? 1 : nth; // parallelize by src1 rows

    const int64_t ith0 = ith % nth0;
    const int64_t ith1 = ith / nth0;

    const int64_t dr0 = (nr0 + nth0 - 1)/nth0;
    const int64_t dr1 = (nr1 + nth1 - 1)/nth1;

    const int64_t ir010 = dr0*ith0;
    const int64_t ir011 = MIN(ir010 + dr0, nr0);

    const int64_t ir110 = dr1*ith1;
    const int64_t ir111 = MIN(ir110 + dr1, nr1);

    //printf("ir010 = %6lld, ir011 = %6lld, ir110 = %6lld, ir111 = %6lld\n", ir010, ir011, ir110, ir111);

    // threads with no work simply yield (not sure if it helps)
    if (ir010 >= ir011 || ir110 >= ir111) {
        sched_yield();
        return;
    }

    assert(ne12 % ne02 == 0);
    assert(ne13 % ne03 == 0);

    // block-tiling attempt
    const int64_t blck_0 = 16;
    const int64_t blck_1 = 16;

    // attempt to reduce false-sharing (does not seem to make a difference)
    float tmp[16];

    for (int64_t iir1 = ir110; iir1 < ir111; iir1 += blck_1) {
        for (int64_t iir0 = ir010; iir0 < ir011; iir0 += blck_0) {
            for (int64_t ir1 = iir1; ir1 < iir1 + blck_1 && ir1 < ir111; ++ir1) {
                const int64_t i13 = (ir1/(ne12*ne11));
                const int64_t i12 = (ir1 - i13*ne12*ne11)/ne11;
                const int64_t i11 = (ir1 - i13*ne12*ne11 - i12*ne11);

                // broadcast src0 into src1
                const int64_t i03 = i13/r3;
                const int64_t i02 = i12/r2;

                const int64_t i1 = i11;
                const int64_t i2 = i12;
                const int64_t i3 = i13;

                const char * src0_row = (const char *) src0->data + (0 + i02*nb02 + i03*nb03);

                // desc: when src1 is not a contiguous memory block we have to calculate the offset using the strides
                //       if it is, then we have either copied the data to params->wdata and made it contiguous or we are using
                //       the original src1 data pointer, so we should index using the indices directly
                // TODO: this is a bit of a hack, we should probably have a better way to handle this
                const char * src1_col = (const char *) wdata +
                    (src1_cont || src1->type != vec_dot_type
                     ? (i11      + i12*ne11 + i13*ne12*ne11)*row_size
                     : (i11*nb11 + i12*nb12 + i13*nb13));

                float * dst_col = (float *) ((char *) dst->data + (i1*nb1 + i2*nb2 + i3*nb3));

                //for (int64_t ir0 = iir0; ir0 < iir0 + blck_0 && ir0 < ir011; ++ir0) {
                //    vec_dot(ne00, &dst_col[ir0], src0_row + ir0*nb01, src1_col);
                //}

                for (int64_t ir0 = iir0; ir0 < iir0 + blck_0 && ir0 < ir011; ++ir0) {
                    vec_dot(ne00, &tmp[ir0 - iir0], src0_row + ir0*nb01, src1_col);
                }
                memcpy(&dst_col[iir0], tmp, (MIN(iir0 + blck_0, ir011) - iir0)*sizeof(float));
            }
        }
    }
}

// lm_ggml_compute_forward_out_prod

static void lm_ggml_compute_forward_out_prod_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    // int64_t t0 = lm_ggml_perf_time_us();
    // UNUSED(t0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    const int ith = params->ith;
    const int nth = params->nth;

    LM_GGML_ASSERT(ne02 == ne12);
    LM_GGML_ASSERT(ne03 == ne13);
    LM_GGML_ASSERT(ne2  == ne12);
    LM_GGML_ASSERT(ne3  == ne13);

    // we don't support permuted src0 or src1
    LM_GGML_ASSERT(nb00 == sizeof(float));

    // dst cannot be transposed or permuted
    LM_GGML_ASSERT(nb0 == sizeof(float));
    // LM_GGML_ASSERT(nb0 <= nb1);
    // LM_GGML_ASSERT(nb1 <= nb2);
    // LM_GGML_ASSERT(nb2 <= nb3);

    LM_GGML_ASSERT(ne0 == ne00);
    LM_GGML_ASSERT(ne1 == ne10);
    LM_GGML_ASSERT(ne2 == ne02);
    LM_GGML_ASSERT(ne3 == ne03);

    // nb01 >= nb00 - src0 is not transposed
    //   compute by src0 rows

    // TODO: #if defined(LM_GGML_USE_CUBLAS) lm_ggml_cuda_out_prod
    // TODO: #if defined(LM_GGML_USE_ACCELERATE) || defined(LM_GGML_USE_OPENBLAS) || defined(LM_GGML_USE_CLBLAST)

    if (params->type == LM_GGML_TASK_INIT) {
        lm_ggml_vec_set_f32(ne0*ne1*ne2*ne3, dst->data, 0);
        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // dst[:,:,:,:] = 0
    // for i2,i3:
    //   for i1:
    //     for i01:
    //       for i0:
    //         dst[i0,i1,i2,i3] += src0[i0,i01,i2,i3] * src1[i1,i01,i2,i3]

    // parallelize by last three dimensions

    // total rows in dst
    const int64_t nr = ne1*ne2*ne3;

    // rows per thread
    const int64_t dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int64_t ir0 = dr*ith;
    const int64_t ir1 = MIN(ir0 + dr, nr);

    // block-tiling attempt
    const int64_t blck_0 = MAX(LM_GGML_VEC_MAD_UNROLL, 32);
    const int64_t blck_1 = 16;

    for (int64_t bir = ir0; bir < ir1; bir += blck_1) {
        const int64_t bir1 = MIN(bir + blck_1, ir1);
        for (int64_t bi01 = 0; bi01 < ne01; bi01 += blck_0) {
            const int64_t bne01 = MIN(bi01 + blck_0, ne01);
            for (int64_t ir = bir; ir < bir1; ++ir) {
                // dst indices
                const int64_t i3 = ir/(ne2*ne1);
                const int64_t i2 = (ir - i3*ne2*ne1)/ne1;
                const int64_t i1 = (ir - i3*ne2*ne1 - i2*ne1);

                const int64_t i02 = i2;
                const int64_t i03 = i3;

                //const int64_t i10 = i1;
                const int64_t i12 = i2;
                const int64_t i13 = i3;

#if LM_GGML_VEC_MAD_UNROLL > 2
                const int64_t bne01_unroll = bne01 - (bne01 % LM_GGML_VEC_MAD_UNROLL);
                for (int64_t i01 = bi01; i01 < bne01_unroll; i01 += LM_GGML_VEC_MAD_UNROLL) {
                    const int64_t i11 = i01;

                    float * s0 = (float *) ((char *) src0->data + (          i01*nb01 + i02*nb02 + i03*nb03));
                    float * s1 = (float *) ((char *) src1->data + (i1*nb10 + i11*nb11 + i12*nb12 + i13*nb13));
                    float * d  = (float *) ((char *)  dst->data + (          i1*nb1 + i2*nb2 + i3*nb3));

                    lm_ggml_vec_mad_f32_unroll(ne0, nb01, nb11, d, s0, s1);
                }
                for (int64_t i01 = bne01_unroll; i01 < bne01; ++i01) {
                    const int64_t i11 = i01;

                    float * s0 = (float *) ((char *) src0->data + (          i01*nb01 + i02*nb02 + i03*nb03));
                    float * s1 = (float *) ((char *) src1->data + (i1*nb10 + i11*nb11 + i12*nb12 + i13*nb13));
                    float * d  = (float *) ((char *)  dst->data + (          i1*nb1 + i2*nb2 + i3*nb3));

                    lm_ggml_vec_mad_f32(ne0, d, s0, *s1);
                }
#else
                for (int64_t i01 = bi01; i01 < bne01; ++i01) {
                    const int64_t i11 = i01;

                    float * s0 = (float *) ((char *) src0->data + (          i01*nb01 + i02*nb02 + i03*nb03));
                    float * s1 = (float *) ((char *) src1->data + (i1*nb10 + i11*nb11 + i12*nb12 + i13*nb13));
                    float * d  = (float *) ((char *)  dst->data + (          i1*nb1 + i2*nb2 + i3*nb3));

                    lm_ggml_vec_mad_f32(ne0, d, s0, *s1);
                }
#endif
            }
        }
    }

    //int64_t t1 = lm_ggml_perf_time_us();
    //static int64_t acc = 0;
    //acc += t1 - t0;
    //if (t1 - t0 > 10) {
    //    printf("\n");
    //    printf("ne00 = %5d, ne01 = %5d, ne02 = %5d, ne03 = %5d\n", ne00, ne01, ne02, ne03);
    //    printf("nb00 = %5d, nb01 = %5d, nb02 = %5d, nb03 = %5d\n", nb00, nb01, nb02, nb03);
    //    printf("ne10 = %5d, ne11 = %5d, ne12 = %5d, ne13 = %5d\n", ne10, ne11, ne12, ne13);
    //    printf("nb10 = %5d, nb11 = %5d, nb12 = %5d, nb13 = %5d\n", nb10, nb11, nb12, nb13);

    //    printf("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX task %d/%d: %d us, acc = %d\n", ith, nth, (int) (t1 - t0), (int) acc);
    //}
}

static void lm_ggml_compute_forward_out_prod_q_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    // int64_t t0 = lm_ggml_perf_time_us();
    // UNUSED(t0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS;

    const int ith = params->ith;
    const int nth = params->nth;

    const enum lm_ggml_type type = src0->type;
    lm_ggml_to_float_t const dequantize_row_q = type_traits[type].to_float;

    LM_GGML_ASSERT(ne02 == ne12);
    LM_GGML_ASSERT(ne03 == ne13);
    LM_GGML_ASSERT(ne2  == ne12);
    LM_GGML_ASSERT(ne3  == ne13);

    // we don't support permuted src0 dim0
    LM_GGML_ASSERT(nb00 == lm_ggml_type_size(type));

    // dst dim0 cannot be transposed or permuted
    LM_GGML_ASSERT(nb0 == sizeof(float));
    // LM_GGML_ASSERT(nb0 <= nb1);
    // LM_GGML_ASSERT(nb1 <= nb2);
    // LM_GGML_ASSERT(nb2 <= nb3);

    LM_GGML_ASSERT(ne0 == ne00);
    LM_GGML_ASSERT(ne1 == ne10);
    LM_GGML_ASSERT(ne2 == ne02);
    LM_GGML_ASSERT(ne3 == ne03);

    // nb01 >= nb00 - src0 is not transposed
    //   compute by src0 rows

    // TODO: #if defined(LM_GGML_USE_CUBLAS) lm_ggml_cuda_out_prod
    // TODO: #if defined(LM_GGML_USE_ACCELERATE) || defined(LM_GGML_USE_OPENBLAS) || defined(LM_GGML_USE_CLBLAST)

    if (params->type == LM_GGML_TASK_INIT) {
        lm_ggml_vec_set_f32(ne0*ne1*ne2*ne3, dst->data, 0);
        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // parallelize by last three dimensions

    // total rows in dst
    const int64_t nr = ne1*ne2*ne3;

    // rows per thread
    const int64_t dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int64_t ir0 = dr*ith;
    const int64_t ir1 = MIN(ir0 + dr, nr);

    // dst[:,:,:,:] = 0
    // for i2,i3:
    //   for i1:
    //     for i01:
    //       for i0:
    //         dst[i0,i1,i2,i3] += src0[i0,i01,i2,i3] * src1[i1,i01,i2,i3]

    float * wdata = (float *) params->wdata + (ne0 + CACHE_LINE_SIZE_F32) * ith;

    for (int64_t ir = ir0; ir < ir1; ++ir) {
        // dst indices
        const int64_t i3 = ir/(ne2*ne1);
        const int64_t i2 = (ir - i3*ne2*ne1)/ne1;
        const int64_t i1 = (ir - i3*ne2*ne1 - i2*ne1);

        const int64_t i02 = i2;
        const int64_t i03 = i3;

        //const int64_t i10 = i1;
        const int64_t i12 = i2;
        const int64_t i13 = i3;

        for (int64_t i01 = 0; i01 < ne01; ++i01) {
            const int64_t i11 = i01;

            float * s0 = (float *) ((char *) src0->data + (          i01*nb01 + i02*nb02 + i03*nb03));
            float * s1 = (float *) ((char *) src1->data + (i1*nb10 + i11*nb11 + i12*nb12 + i13*nb13));
            float * d  = (float *) ((char *)  dst->data + (          i1*nb1 + i2*nb2 + i3*nb3));

            dequantize_row_q(s0, wdata, ne0);
            lm_ggml_vec_mad_f32(ne0, d, wdata, *s1);
        }
    }

    //int64_t t1 = lm_ggml_perf_time_us();
    //static int64_t acc = 0;
    //acc += t1 - t0;
    //if (t1 - t0 > 10) {
    //    printf("\n");
    //    printf("ne00 = %5d, ne01 = %5d, ne02 = %5d, ne03 = %5d\n", ne00, ne01, ne02, ne03);
    //    printf("nb00 = %5d, nb01 = %5d, nb02 = %5d, nb03 = %5d\n", nb00, nb01, nb02, nb03);
    //    printf("ne10 = %5d, ne11 = %5d, ne12 = %5d, ne13 = %5d\n", ne10, ne11, ne12, ne13);
    //    printf("nb10 = %5d, nb11 = %5d, nb12 = %5d, nb13 = %5d\n", nb10, nb11, nb12, nb13);

    //    printf("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX task %d/%d: %d us, acc = %d\n", ith, nth, (int) (t1 - t0), (int) acc);
    //}
}

static void lm_ggml_compute_forward_out_prod(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_Q4_0:
        case LM_GGML_TYPE_Q4_1:
        case LM_GGML_TYPE_Q5_0:
        case LM_GGML_TYPE_Q5_1:
        case LM_GGML_TYPE_Q8_0:
        case LM_GGML_TYPE_Q2_K:
        case LM_GGML_TYPE_Q3_K:
        case LM_GGML_TYPE_Q4_K:
        case LM_GGML_TYPE_Q5_K:
        case LM_GGML_TYPE_Q6_K:
            {
                lm_ggml_compute_forward_out_prod_q_f32(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F16:
            {
                LM_GGML_ASSERT(false); // todo
                // lm_ggml_compute_forward_out_prod_f16_f32(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_out_prod_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_scale

static void lm_ggml_compute_forward_scale_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_is_contiguous(src0));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(dst));
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));
    LM_GGML_ASSERT(lm_ggml_is_scalar(src1));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // scale factor
    const float v = *(float *) src1->data;

    const int ith = params->ith;
    const int nth = params->nth;

    const int nc = src0->ne[0];
    const int nr = lm_ggml_nrows(src0);

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    const size_t nb01 = src0->nb[1];

    const size_t nb1 = dst->nb[1];

    for (int i1 = ir0; i1 < ir1; i1++) {
        if (dst->data != src0->data) {
            // src0 is same shape as dst => same indices
            memcpy((char *)dst->data + i1*nb1, (char *)src0->data + i1*nb01, nc * sizeof(float));
        }
        lm_ggml_vec_scale_f32(nc, (float *) ((char *) dst->data + i1*nb1), v);
    }
}

static void lm_ggml_compute_forward_scale(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_scale_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_set

static void lm_ggml_compute_forward_set_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(dst) && lm_ggml_is_contiguous(src0));

    // view src0 and dst with these strides and data offset inbytes during set
    // nb0 is implicitely element_size because src0 and dst are contiguous
    size_t nb1     = ((int32_t *) dst->op_params)[0];
    size_t nb2     = ((int32_t *) dst->op_params)[1];
    size_t nb3     = ((int32_t *) dst->op_params)[2];
    size_t offset  = ((int32_t *) dst->op_params)[3];
    bool   inplace = (bool) ((int32_t *) dst->op_params)[4];

    if (!inplace && (params->type == LM_GGML_TASK_INIT)) {
        // memcpy needs to be synchronized across threads to avoid race conditions.
        // => do it in INIT phase
        memcpy(
            ((char *)  dst->data),
            ((char *) src0->data),
            lm_ggml_nbytes(dst));
    }

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int ith = params->ith;
    const int nth = params->nth;

    const int nr = lm_ggml_nrows(src1);
    const int nc = src1->ne[0];

    LM_GGML_TENSOR_LOCALS(int64_t, ne1, src1, ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nb1, src1, nb)

    // src0 and dst as viewed during set
    const size_t nb0 = lm_ggml_element_size(src0);

    const int im0 = (ne10 == 0 ? 0 : ne10-1);
    const int im1 = (ne11 == 0 ? 0 : ne11-1);
    const int im2 = (ne12 == 0 ? 0 : ne12-1);
    const int im3 = (ne13 == 0 ? 0 : ne13-1);

    LM_GGML_ASSERT(offset + im0*nb0  + im1*nb1  + im2*nb2  + im3*nb3  <= lm_ggml_nbytes(dst));

    LM_GGML_ASSERT(nb10 == sizeof(float));

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    for (int ir = ir0; ir < ir1; ++ir) {
        // src0 and dst are viewed with shape of src1 and offset
        // => same indices
        const int i3 = ir/(ne12*ne11);
        const int i2 = (ir - i3*ne12*ne11)/ne11;
        const int i1 = (ir - i3*ne12*ne11 - i2*ne11);

        lm_ggml_vec_cpy_f32(nc,
                (float *) ((char *)  dst->data + i3*nb3  + i2*nb2  + i1*nb1  + offset),
                (float *) ((char *) src1->data + i3*nb13 + i2*nb12 + i1*nb11));
    }
}

static void lm_ggml_compute_forward_set(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {

    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_set_f32(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F16:
        case LM_GGML_TYPE_Q4_0:
        case LM_GGML_TYPE_Q4_1:
        case LM_GGML_TYPE_Q5_0:
        case LM_GGML_TYPE_Q5_1:
        case LM_GGML_TYPE_Q8_0:
        case LM_GGML_TYPE_Q8_1:
        case LM_GGML_TYPE_Q2_K:
        case LM_GGML_TYPE_Q3_K:
        case LM_GGML_TYPE_Q4_K:
        case LM_GGML_TYPE_Q5_K:
        case LM_GGML_TYPE_Q6_K:
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_cpy

static void lm_ggml_compute_forward_cpy(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    lm_ggml_compute_forward_dup(params, src0, dst);
}

// lm_ggml_compute_forward_cont

static void lm_ggml_compute_forward_cont(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    lm_ggml_compute_forward_dup(params, src0, dst);
}

// lm_ggml_compute_forward_reshape

static void lm_ggml_compute_forward_reshape(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    // NOP
    UNUSED(params);
    UNUSED(src0);
    UNUSED(dst);
}

// lm_ggml_compute_forward_view

static void lm_ggml_compute_forward_view(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0) {
    // NOP
    UNUSED(params);
    UNUSED(src0);
}

// lm_ggml_compute_forward_permute

static void lm_ggml_compute_forward_permute(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0) {
    // NOP
    UNUSED(params);
    UNUSED(src0);
}

// lm_ggml_compute_forward_transpose

static void lm_ggml_compute_forward_transpose(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0) {
    // NOP
    UNUSED(params);
    UNUSED(src0);
}

// lm_ggml_compute_forward_get_rows

static void lm_ggml_compute_forward_get_rows_q(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int nc = src0->ne[0];
    const int nr = lm_ggml_nelements(src1);
    const enum lm_ggml_type type = src0->type;
    lm_ggml_to_float_t const dequantize_row_q = type_traits[type].to_float;

    assert( dst->ne[0] == nc);
    assert( dst->ne[1] == nr);
    assert(src0->nb[0] == lm_ggml_type_size(type));

    for (int i = 0; i < nr; ++i) {
        const int r = ((int32_t *) src1->data)[i];

        dequantize_row_q(
                (const void *) ((char *) src0->data + r*src0->nb[1]),
                     (float *) ((char *)  dst->data + i*dst->nb[1]), nc);
    }
}

static void lm_ggml_compute_forward_get_rows_f16(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int nc = src0->ne[0];
    const int nr = lm_ggml_nelements(src1);

    assert( dst->ne[0] == nc);
    assert( dst->ne[1] == nr);
    assert(src0->nb[0] == sizeof(lm_ggml_fp16_t));

    for (int i = 0; i < nr; ++i) {
        const int r = ((int32_t *) src1->data)[i];

        for (int j = 0; j < nc; ++j) {
            lm_ggml_fp16_t v = ((lm_ggml_fp16_t *) ((char *) src0->data + r*src0->nb[1]))[j];
            ((float *) ((char *)  dst->data + i*dst->nb[1]))[j] = LM_GGML_FP16_TO_FP32(v);
        }
    }
}

static void lm_ggml_compute_forward_get_rows_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int nc = src0->ne[0];
    const int nr = lm_ggml_nelements(src1);

    assert( dst->ne[0] == nc);
    assert( dst->ne[1] == nr);
    assert(src0->nb[0] == sizeof(float));

    for (int i = 0; i < nr; ++i) {
        const int r = ((int32_t *) src1->data)[i];

        lm_ggml_vec_cpy_f32(nc,
                (float *) ((char *)  dst->data + i*dst->nb[1]),
                (float *) ((char *) src0->data + r*src0->nb[1]));
    }
}

static void lm_ggml_compute_forward_get_rows(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_Q4_0:
        case LM_GGML_TYPE_Q4_1:
        case LM_GGML_TYPE_Q5_0:
        case LM_GGML_TYPE_Q5_1:
        case LM_GGML_TYPE_Q8_0:
        case LM_GGML_TYPE_Q8_1:
        case LM_GGML_TYPE_Q2_K:
        case LM_GGML_TYPE_Q3_K:
        case LM_GGML_TYPE_Q4_K:
        case LM_GGML_TYPE_Q5_K:
        case LM_GGML_TYPE_Q6_K:
            {
                lm_ggml_compute_forward_get_rows_q(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_get_rows_f16(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_get_rows_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }

    //static bool first = true;
    //printf("ne0 = %d, ne1 = %d, ne2 = %d\n", dst->ne[0], dst->ne[1], dst->ne[2]);
    //if (first) {
    //    first = false;
    //} else {
    //    for (int k = 0; k < dst->ne[1]; ++k) {
    //        for (int j = 0; j < dst->ne[0]/16; ++j) {
    //            for (int i = 0; i < 16; ++i) {
    //                printf("%8.4f ", ((float *) dst->data)[k*dst->ne[0] + j*16 + i]);
    //            }
    //            printf("\n");
    //        }
    //        printf("\n");
    //    }
    //    printf("\n");
    //    exit(0);
    //}
}

// lm_ggml_compute_forward_get_rows_back

static void lm_ggml_compute_forward_get_rows_back_f32_f16(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(params->ith == 0);
    LM_GGML_ASSERT(lm_ggml_is_contiguous(dst));

    // lm_ggml_compute_forward_dup_same_cont(params, opt0, dst);

    if (params->type == LM_GGML_TASK_INIT) {
        memset(dst->data, 0, lm_ggml_nbytes(dst));
    }

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int nc = src0->ne[0];
    const int nr = lm_ggml_nelements(src1);

    LM_GGML_ASSERT( dst->ne[0] == nc);
    LM_GGML_ASSERT(src0->nb[0] == sizeof(lm_ggml_fp16_t));

    for (int i = 0; i < nr; ++i) {
        const int r = ((int32_t *) src1->data)[i];

        for (int j = 0; j < nc; ++j) {
            lm_ggml_fp16_t v = ((lm_ggml_fp16_t *) ((char *) src0->data + i*src0->nb[1]))[j];
            ((float *) ((char *) dst->data + r*dst->nb[1]))[j] += LM_GGML_FP16_TO_FP32(v);
        }
    }
}

static void lm_ggml_compute_forward_get_rows_back_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(params->ith == 0);
    LM_GGML_ASSERT(lm_ggml_is_contiguous(dst));

    // lm_ggml_compute_forward_dup_same_cont(params, opt0, dst);

    if (params->type == LM_GGML_TASK_INIT) {
        memset(dst->data, 0, lm_ggml_nbytes(dst));
    }

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int nc = src0->ne[0];
    const int nr = lm_ggml_nelements(src1);

    LM_GGML_ASSERT( dst->ne[0] == nc);
    LM_GGML_ASSERT(src0->nb[0] == sizeof(float));

    for (int i = 0; i < nr; ++i) {
        const int r = ((int32_t *) src1->data)[i];

        lm_ggml_vec_add_f32(nc,
                (float *) ((char *)  dst->data + r*dst->nb[1]),
                (float *) ((char *)  dst->data + r*dst->nb[1]),
                (float *) ((char *) src0->data + i*src0->nb[1]));
    }
}

static void lm_ggml_compute_forward_get_rows_back(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_get_rows_back_f32_f16(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_get_rows_back_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }

    //static bool first = true;
    //printf("ne0 = %d, ne1 = %d, ne2 = %d\n", dst->ne[0], dst->ne[1], dst->ne[2]);
    //if (first) {
    //    first = false;
    //} else {
    //    for (int k = 0; k < dst->ne[1]; ++k) {
    //        for (int j = 0; j < dst->ne[0]/16; ++j) {
    //            for (int i = 0; i < 16; ++i) {
    //                printf("%8.4f ", ((float *) dst->data)[k*dst->ne[0] + j*16 + i]);
    //            }
    //            printf("\n");
    //        }
    //        printf("\n");
    //    }
    //    printf("\n");
    //    exit(0);
    //}
}

// lm_ggml_compute_forward_diag

static void lm_ggml_compute_forward_diag_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // TODO: handle transposed/permuted matrices

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    LM_GGML_ASSERT(ne00 == ne0);
    LM_GGML_ASSERT(ne00 == ne1);
    LM_GGML_ASSERT(ne01 == 1);
    LM_GGML_ASSERT(ne02 == ne2);
    LM_GGML_ASSERT(ne03 == ne3);

    LM_GGML_ASSERT(nb00 == sizeof(float));
    LM_GGML_ASSERT(nb0  == sizeof(float));

    for (int i3 = 0; i3 < ne3; i3++) {
        for (int i2 = 0; i2 < ne2; i2++) {
            for (int i1 = 0; i1 < ne1; i1++) {
                float * d = (float *)((char *)  dst->data + i3*nb3  + i2*nb2 + i1*nb1);
                float * s = (float *)((char *) src0->data + i3*nb03 + i2*nb02);
                for (int i0 = 0; i0 < i1; i0++) {
                    d[i0] = 0;
                }
                d[i1] = s[i1];
                for (int i0 = i1+1; i0 < ne0; i0++) {
                    d[i0] = 0;
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_diag(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_diag_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_diag_mask_inf

static void lm_ggml_compute_forward_diag_mask_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst,
        const float value) {

    const int ith = params->ith;
    const int nth = params->nth;

    const int  n_past  = ((int32_t *) dst->op_params)[0];
    const bool inplace = src0->data == dst->data;

    LM_GGML_ASSERT(n_past >= 0);

    if (!inplace && (params->type == LM_GGML_TASK_INIT)) {
        // memcpy needs to be synchronized across threads to avoid race conditions.
        // => do it in INIT phase
        LM_GGML_ASSERT(lm_ggml_nelements(dst) == lm_ggml_nelements(src0));
        LM_GGML_ASSERT(lm_ggml_is_contiguous(dst) && lm_ggml_is_contiguous(src0));
        memcpy(
            ((char *)  dst->data),
            ((char *) src0->data),
            lm_ggml_nbytes(dst));
    }

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // TODO: handle transposed/permuted matrices

    const int n  = lm_ggml_nrows(src0);
    const int nc = src0->ne[0];
    const int nr = src0->ne[1];
    const int nz = n/nr;

    LM_GGML_ASSERT( dst->nb[0] == sizeof(float));
    LM_GGML_ASSERT(src0->nb[0] == sizeof(float));

    for (int k = 0; k < nz; k++) {
        for (int j = ith; j < nr; j += nth) {
            for (int i = n_past; i < nc; i++) {
                if (i > n_past + j) {
                    *(float *)((char *) dst->data + k*dst->nb[2] + j*dst->nb[1] + i*dst->nb[0]) = value;
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_diag_mask_inf(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_diag_mask_f32(params, src0, dst, -INFINITY);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

static void lm_ggml_compute_forward_diag_mask_zero(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_diag_mask_f32(params, src0, dst, 0);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_soft_max

static void lm_ggml_compute_forward_soft_max_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_is_contiguous(src0));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(dst));
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // TODO: handle transposed/permuted matrices

    const int ith = params->ith;
    const int nth = params->nth;

    const int nc = src0->ne[0];
    const int nr = lm_ggml_nrows(src0);

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    for (int i1 = ir0; i1 < ir1; i1++) {
        float *sp = (float *)((char *) src0->data + i1*src0->nb[1]);
        float *dp = (float *)((char *)  dst->data +  i1*dst->nb[1]);

#ifndef NDEBUG
        for (int i = 0; i < nc; ++i) {
            //printf("p[%d] = %f\n", i, p[i]);
            assert(!isnan(sp[i]));
        }
#endif

        float max = -INFINITY;
        lm_ggml_vec_max_f32(nc, &max, sp);

        lm_ggml_float sum = 0.0;

        uint16_t scvt;
        for (int i = 0; i < nc; i++) {
            if (sp[i] == -INFINITY) {
                dp[i] = 0.0f;
            } else {
                // const float val = (sp[i] == -INFINITY) ? 0.0 : exp(sp[i] - max);
                lm_ggml_fp16_t s = LM_GGML_FP32_TO_FP16(sp[i] - max);
                memcpy(&scvt, &s, sizeof(scvt));
                const float val = LM_GGML_FP16_TO_FP32(table_exp_f16[scvt]);
                sum += (lm_ggml_float)val;
                dp[i] = val;
            }
        }

        assert(sum > 0.0);

        sum = 1.0/sum;
        lm_ggml_vec_scale_f32(nc, dp, sum);

#ifndef NDEBUG
        for (int i = 0; i < nc; ++i) {
            assert(!isnan(dp[i]));
            assert(!isinf(dp[i]));
        }
#endif
    }
}

static void lm_ggml_compute_forward_soft_max(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_soft_max_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_soft_max_back

static void lm_ggml_compute_forward_soft_max_back_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_is_contiguous(src0));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(src1));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(dst));
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src1, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // TODO: handle transposed/permuted matrices

    const int ith = params->ith;
    const int nth = params->nth;

    const int nc = src0->ne[0];
    const int nr = lm_ggml_nrows(src0);

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    for (int i1 = ir0; i1 < ir1; i1++) {
        float *dy = (float *)((char *) src0->data + i1*src0->nb[1]);
        float *y  = (float *)((char *) src1->data + i1*src1->nb[1]);
        float *dx = (float *)((char *) dst->data  + i1*dst->nb[1]);

#ifndef NDEBUG
        for (int i = 0; i < nc; ++i) {
            //printf("p[%d] = %f\n", i, p[i]);
            assert(!isnan(dy[i]));
            assert(!isnan(y[i]));
        }
#endif
        // Jii = yi - yi*yi
        // Jij = -yi*yj
        // J = diag(y)-y.T*y
        // dx = J * dy
        // dxk = sum_i(Jki * dyi)
        // dxk = sum_i(-yk*yi * dyi) - (-yk*yk)*dyk + (yk - yk*yk)*dyk
        // dxk = sum_i(-yk*yi * dyi) + yk*yk*dyk + yk*dyk - yk*yk*dyk
        // dxk = sum_i(-yk*yi * dyi) + yk*dyk
        // dxk = -yk * sum_i(yi * dyi) + yk*dyk
        // dxk = -yk * dot(y, dy) + yk*dyk
        // dxk = yk * (- dot(y, dy) + dyk)
        // dxk = yk * (dyk - dot(y, dy))
        //
        // post-order:
        // dot_y_dy := dot(y, dy)
        // dx := dy
        // dx := dx - dot_y_dy
        // dx := dx * y

        // linear runtime, no additional memory
        float dot_y_dy = 0;
        lm_ggml_vec_dot_f32 (nc, &dot_y_dy, y, dy);
        lm_ggml_vec_cpy_f32 (nc, dx, dy);
        lm_ggml_vec_acc1_f32(nc, dx, -dot_y_dy);
        lm_ggml_vec_mul_f32 (nc, dx, dx, y);

#ifndef NDEBUG
        for (int i = 0; i < nc; ++i) {
            assert(!isnan(dx[i]));
            assert(!isinf(dx[i]));
        }
#endif
    }
}

static void lm_ggml_compute_forward_soft_max_back(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_soft_max_back_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_alibi

static void lm_ggml_compute_forward_alibi_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    //const int n_past = ((int32_t *) dst->op_params)[0];
    const int n_head = ((int32_t *) dst->op_params)[1];
    float max_bias;
    memcpy(&max_bias, (int32_t *) dst->op_params + 2, sizeof(float));

    const int64_t ne0 = src0->ne[0]; // all_seq_len = n_past + ne1
    const int64_t ne1 = src0->ne[1]; // seq_len_without_past
    const int64_t ne2 = src0->ne[2]; // n_head -> this is k
    //const int64_t ne3 = src0->ne[3]; // 1 -> bsz

    const int64_t n  = lm_ggml_nrows(src0);
    const int64_t ne2_ne3 = n/ne1; // ne2*ne3

    const size_t nb0 = src0->nb[0];
    const size_t nb1 = src0->nb[1];
    const size_t nb2 = src0->nb[2];
    //const int nb3 = src0->nb[3];

    LM_GGML_ASSERT(nb0 == sizeof(float));
    LM_GGML_ASSERT(n_head == ne2);

    // add alibi to src0 (KQ_scaled)
    const int n_heads_log2_floor = 1 << (int) floor(log2(n_head));

    const float m0 = powf(2.0f, -(max_bias) / n_heads_log2_floor);
    const float m1 = powf(2.0f, -(max_bias / 2.0f) / n_heads_log2_floor);

    for (int64_t i = 0; i < ne0; i++) {
        for (int64_t j = 0; j < ne1; j++) {
            for (int64_t k = 0; k < ne2_ne3; k++) {
                float * const src = (float *)((char *) src0->data + i*nb0 + j*nb1 + k*nb2);
                float *      pdst = (float *)((char *)  dst->data + i*nb0 + j*nb1 + k*nb2);

                // TODO: k*nb2 or k*nb3

                float m_k;

                if (k < n_heads_log2_floor) {
                    m_k = powf(m0, k + 1);
                } else {
                    m_k = powf(m1, 2 * (k - n_heads_log2_floor) + 1);
                }

                pdst[0] = i * m_k + src[0];
            }
        }
    }
}

static void lm_ggml_compute_forward_alibi_f16(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    //const int n_past = ((int32_t *) dst->op_params)[0];
    const int n_head = ((int32_t *) dst->op_params)[1];
    float max_bias;
    memcpy(&max_bias, (int32_t *) dst->op_params + 2, sizeof(float));

    const int ne0 = src0->ne[0]; // all_seq_len = n_past + ne1
    const int ne1 = src0->ne[1]; // seq_len_without_past
    const int ne2 = src0->ne[2]; // n_head -> this is k
    //const int ne3 = src0->ne[3]; // 1 -> bsz

    const int n  = lm_ggml_nrows(src0);
    const int ne2_ne3 = n/ne1; // ne2*ne3

    const int nb0 = src0->nb[0];
    const int nb1 = src0->nb[1];
    const int nb2 = src0->nb[2];
    //const int nb3 = src0->nb[3];

    LM_GGML_ASSERT(nb0 == sizeof(lm_ggml_fp16_t));
    //LM_GGML_ASSERT(ne1 + n_past == ne0); (void) n_past;
    LM_GGML_ASSERT(n_head == ne2);

    // add alibi to src0 (KQ_scaled)
    const int n_heads_log2_floor = 1 << (int) floor(log2(n_head));

    const float m0 = powf(2.0f, -(max_bias) / n_heads_log2_floor);
    const float m1 = powf(2.0f, -(max_bias / 2.0f) / n_heads_log2_floor);

    for (int i = 0; i < ne0; i++) {
        for (int j = 0; j < ne1; j++) {
            for (int k = 0; k < ne2_ne3; k++) {
                lm_ggml_fp16_t * const src  = (lm_ggml_fp16_t *)((char *) src0->data + i*nb0 + j*nb1 + k*nb2);
                      float *      pdst  =       (float *)((char *)  dst->data + i*nb0 + j*nb1 + k*nb2);

                // TODO: k*nb2 or k*nb3

                float m_k;

                if (k < n_heads_log2_floor) {
                    m_k = powf(m0, k + 1);
                } else {
                    m_k = powf(m1, 2 * (k - n_heads_log2_floor) + 1);
                }

                // we return F32
                pdst[0] = i * m_k + LM_GGML_FP16_TO_FP32(src[0]);
            }
        }
    }
}

static void lm_ggml_compute_forward_alibi(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_alibi_f16(params, src0, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_alibi_f32(params, src0, dst);
            } break;
        case LM_GGML_TYPE_Q4_0:
        case LM_GGML_TYPE_Q4_1:
        case LM_GGML_TYPE_Q5_0:
        case LM_GGML_TYPE_Q5_1:
        case LM_GGML_TYPE_Q8_0:
        case LM_GGML_TYPE_Q8_1:
        case LM_GGML_TYPE_Q2_K:
        case LM_GGML_TYPE_Q3_K:
        case LM_GGML_TYPE_Q4_K:
        case LM_GGML_TYPE_Q5_K:
        case LM_GGML_TYPE_Q6_K:
        case LM_GGML_TYPE_Q8_K:
        case LM_GGML_TYPE_I8:
        case LM_GGML_TYPE_I16:
        case LM_GGML_TYPE_I32:
        case LM_GGML_TYPE_COUNT:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_clamp

static void lm_ggml_compute_forward_clamp_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    assert(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    float min;
    float max;
    memcpy(&min, (float *) dst->op_params + 0, sizeof(float));
    memcpy(&max, (float *) dst->op_params + 1, sizeof(float));

    const int ith = params->ith;
    const int nth = params->nth;

    const int n  = lm_ggml_nrows(src0);
    const int nc = src0->ne[0];

    const size_t nb00 = src0->nb[0];
    const size_t nb01 = src0->nb[1];

    const size_t nb0 = dst->nb[0];
    const size_t nb1 = dst->nb[1];

    LM_GGML_ASSERT( nb0 == sizeof(float));
    LM_GGML_ASSERT(nb00 == sizeof(float));

    for (int j = ith; j < n; j += nth) {
        float * dst_ptr  = (float *) ((char *)  dst->data + j*nb1);
        float * src0_ptr = (float *) ((char *) src0->data + j*nb01);

        for (int i = 0; i < nc; i++) {
            dst_ptr[i] = MAX(MIN(src0_ptr[i], max), min);
        }
    }
}

static void lm_ggml_compute_forward_clamp(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_clamp_f32(params, src0, dst);
            } break;
        case LM_GGML_TYPE_F16:
        case LM_GGML_TYPE_Q4_0:
        case LM_GGML_TYPE_Q4_1:
        case LM_GGML_TYPE_Q5_0:
        case LM_GGML_TYPE_Q5_1:
        case LM_GGML_TYPE_Q8_0:
        case LM_GGML_TYPE_Q8_1:
        case LM_GGML_TYPE_Q2_K:
        case LM_GGML_TYPE_Q3_K:
        case LM_GGML_TYPE_Q4_K:
        case LM_GGML_TYPE_Q5_K:
        case LM_GGML_TYPE_Q6_K:
        case LM_GGML_TYPE_Q8_K:
        case LM_GGML_TYPE_I8:
        case LM_GGML_TYPE_I16:
        case LM_GGML_TYPE_I32:
        case LM_GGML_TYPE_COUNT:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_rope

static void lm_ggml_compute_forward_rope_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    float freq_base;
    float freq_scale;

    // these two only relevant for xPos RoPE:
    float xpos_base;
    bool  xpos_down;

    //const int n_past = ((int32_t *) dst->op_params)[0];
    const int n_dims = ((int32_t *) dst->op_params)[1];
    const int mode   = ((int32_t *) dst->op_params)[2];
    const int n_ctx  = ((int32_t *) dst->op_params)[3];
    memcpy(&freq_base,  (int32_t *) dst->op_params + 4, sizeof(float));
    memcpy(&freq_scale, (int32_t *) dst->op_params + 5, sizeof(float));
    memcpy(&xpos_base,  (int32_t *) dst->op_params + 6, sizeof(float));
    memcpy(&xpos_down,  (int32_t *) dst->op_params + 7, sizeof(bool));

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    //printf("ne0: %d, ne1: %d, ne2: %d, ne3: %d\n", ne0, ne1, ne2, ne3);
    //printf("n_past = %d, ne2 = %d\n", n_past, ne2);

    LM_GGML_ASSERT(nb00 == sizeof(float));

    const int ith = params->ith;
    const int nth = params->nth;

    const int nr = lm_ggml_nrows(dst);

    LM_GGML_ASSERT(n_dims <= ne0);
    LM_GGML_ASSERT(n_dims % 2 == 0);

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    // row index used to determine which thread to use
    int ir = 0;

    const float theta_scale = powf(freq_base, -2.0f/n_dims);

    const bool is_neox = mode & 2;
    const bool is_glm  = mode & 4;

    const int32_t * pos = (const int32_t *) src1->data;

    for (int64_t i3 = 0; i3 < ne3; i3++) {
        for (int64_t i2 = 0; i2 < ne2; i2++) {
            const int64_t p = pos[i2];
            for (int64_t i1 = 0; i1 < ne1; i1++) {
                if (ir++ < ir0) continue;
                if (ir   > ir1) break;

                float theta = freq_scale * (float)p;

                if (is_glm) {
                    theta = MIN(p, n_ctx - 2);
                    float block_theta = MAX(p - (n_ctx - 2), 0);
                    for (int64_t i0 = 0; i0 < ne0 / 4; i0++) {
                        const float cos_theta = cosf(theta);
                        const float sin_theta = sinf(theta);
                        const float cos_block_theta = cosf(block_theta);
                        const float sin_block_theta = sinf(block_theta);

                        theta *= theta_scale;
                        block_theta *= theta_scale;

                        const float * const src = (float *)((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01 + i0*nb00);
                              float * dst_data  = (float *)((char *)  dst->data +  i3*nb3 + i2*nb2  + i1*nb1  + i0*nb0);

                        const float x0 = src[0];
                        const float x1 = src[n_dims/2];
                        const float x2 = src[n_dims];
                        const float x3 = src[n_dims/2*3];

                        dst_data[0]          = x0*cos_theta - x1*sin_theta;
                        dst_data[n_dims/2]   = x0*sin_theta + x1*cos_theta;
                        dst_data[n_dims]     = x2*cos_block_theta - x3*sin_block_theta;
                        dst_data[n_dims/2*3] = x2*sin_block_theta + x3*cos_block_theta;
                    }
                } else if (!is_neox) {
                    for (int64_t i0 = 0; i0 < ne0; i0 += 2) {
                        const float cos_theta = cosf(theta);
                        const float sin_theta = sinf(theta);
                        // zeta scaling for xPos only:
                        float zeta = xpos_base != 0.0f ? powf((i0 + 0.4f * ne0) / (1.4f * ne0), p / xpos_base) : 1.0f;
                        if (xpos_down) zeta = 1.0f / zeta;

                        theta *= theta_scale;

                        const float * const src = (float *)((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01 + i0*nb00);
                              float * dst_data  = (float *)((char *)  dst->data + i3*nb3  + i2*nb2  + i1*nb1  + i0*nb0);

                        const float x0 = src[0];
                        const float x1 = src[1];

                        dst_data[0] = x0*cos_theta*zeta - x1*sin_theta*zeta;
                        dst_data[1] = x0*sin_theta*zeta + x1*cos_theta*zeta;
                    }
                } else {
                    // TODO: this might be wrong for ne0 != n_dims - need double check
                    // ref:  https://github.com/huggingface/transformers/blob/main/src/transformers/models/gpt_neox/modeling_gpt_neox.py#LL251C1-L294C28
                    for (int64_t ib = 0; ib < ne0/n_dims; ++ib) {
                        for (int64_t ic = 0; ic < n_dims; ic += 2) {
                            const float cos_theta = cosf(theta);
                            const float sin_theta = sinf(theta);

                            theta *= theta_scale;

                            const int64_t i0 = ib*n_dims + ic/2;

                            const float * const src = (float *)((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01 + i0*nb00);
                                  float * dst_data  = (float *)((char *)  dst->data + i3*nb3  + i2*nb2  + i1*nb1  + i0*nb0);

                            const float x0 = src[0];
                            const float x1 = src[n_dims/2];

                            dst_data[0]        = x0*cos_theta - x1*sin_theta;
                            dst_data[n_dims/2] = x0*sin_theta + x1*cos_theta;
                        }
                    }
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_rope_f16(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    float freq_base;
    float freq_scale;

    //const int n_past = ((int32_t *) dst->op_params)[0];
    const int n_dims = ((int32_t *) dst->op_params)[1];
    const int mode   = ((int32_t *) dst->op_params)[2];
    const int n_ctx  = ((int32_t *) dst->op_params)[3];
    memcpy(&freq_base,  (int32_t *) dst->op_params + 4, sizeof(float));
    memcpy(&freq_scale, (int32_t *) dst->op_params + 5, sizeof(float));

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    //printf("ne0: %d, ne1: %d, ne2: %d, ne3: %d\n", ne0, ne1, ne2, ne3);
    //printf("n_past = %d, ne2 = %d\n", n_past, ne2);

    LM_GGML_ASSERT(nb0 == sizeof(lm_ggml_fp16_t));

    const int ith = params->ith;
    const int nth = params->nth;

    const int nr = lm_ggml_nrows(dst);

    LM_GGML_ASSERT(n_dims <= ne0);
    LM_GGML_ASSERT(n_dims % 2 == 0);

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    // row index used to determine which thread to use
    int ir = 0;

    const float theta_scale = powf(freq_base, -2.0f/n_dims);

    const bool is_neox = mode & 2;
    const bool is_glm  = mode & 4;

    const int32_t * pos = (const int32_t *) src1->data;

    for (int64_t i3 = 0; i3 < ne3; i3++) {
        for (int64_t i2 = 0; i2 < ne2; i2++) {
            const int64_t p = pos[i2];
            for (int64_t i1 = 0; i1 < ne1; i1++) {
                if (ir++ < ir0) continue;
                if (ir   > ir1) break;

                float theta = freq_scale * (float)p;

                if (is_glm) {
                    theta = MIN(p, n_ctx - 2);
                    float block_theta = MAX(p - (n_ctx - 2), 0);
                    for (int64_t i0 = 0; i0 < ne0 / 4; i0++) {
                        const float cos_theta = cosf(theta);
                        const float sin_theta = sinf(theta);
                        const float cos_block_theta = cosf(block_theta);
                        const float sin_block_theta = sinf(block_theta);

                        theta *= theta_scale;
                        block_theta *= theta_scale;

                        const lm_ggml_fp16_t * const src = (lm_ggml_fp16_t *)((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01 + i0*nb00);
                              lm_ggml_fp16_t * dst_data  = (lm_ggml_fp16_t *)((char *)  dst->data +  i3*nb3 + i2*nb2  + i1*nb1  + i0*nb0);

                        const float x0 = LM_GGML_FP16_TO_FP32(src[0]);
                        const float x1 = LM_GGML_FP16_TO_FP32(src[n_dims/2]);
                        const float x2 = LM_GGML_FP16_TO_FP32(src[n_dims]);
                        const float x3 = LM_GGML_FP16_TO_FP32(src[n_dims/2*3]);

                        dst_data[0]          = LM_GGML_FP32_TO_FP16(x0*cos_theta - x1*sin_theta);
                        dst_data[n_dims/2]   = LM_GGML_FP32_TO_FP16(x0*sin_theta + x1*cos_theta);
                        dst_data[n_dims]     = LM_GGML_FP32_TO_FP16(x2*cos_block_theta - x3*sin_block_theta);
                        dst_data[n_dims/2*3] = LM_GGML_FP32_TO_FP16(x2*sin_block_theta + x3*cos_block_theta);
                    }
                } else if (!is_neox) {
                    for (int64_t i0 = 0; i0 < ne0; i0 += 2) {
                        const float cos_theta = cosf(theta);
                        const float sin_theta = sinf(theta);

                        theta *= theta_scale;

                        const lm_ggml_fp16_t * const src = (lm_ggml_fp16_t *)((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01 + i0*nb00);
                              lm_ggml_fp16_t * dst_data  = (lm_ggml_fp16_t *)((char *)  dst->data + i3*nb3  + i2*nb2  + i1*nb1  + i0*nb0);

                        const float x0 = LM_GGML_FP16_TO_FP32(src[0]);
                        const float x1 = LM_GGML_FP16_TO_FP32(src[1]);

                        dst_data[0] = LM_GGML_FP32_TO_FP16(x0*cos_theta - x1*sin_theta);
                        dst_data[1] = LM_GGML_FP32_TO_FP16(x0*sin_theta + x1*cos_theta);
                    }
                } else {
                    // TODO: this might be wrong for ne0 != n_dims - need double check
                    // ref:  https://github.com/huggingface/transformers/blob/main/src/transformers/models/gpt_neox/modeling_gpt_neox.py#LL251C1-L294C28
                    for (int64_t ib = 0; ib < ne0/n_dims; ++ib) {
                        for (int64_t ic = 0; ic < n_dims; ic += 2) {
                            const float cos_theta = cosf(theta);
                            const float sin_theta = sinf(theta);

                            theta *= theta_scale;

                            const int64_t i0 = ib*n_dims + ic/2;

                            const lm_ggml_fp16_t * const src = (lm_ggml_fp16_t *)((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01 + i0*nb00);
                                  lm_ggml_fp16_t * dst_data  = (lm_ggml_fp16_t *)((char *)  dst->data + i3*nb3  + i2*nb2  + i1*nb1  + i0*nb0);

                            const float x0 = LM_GGML_FP16_TO_FP32(src[0]);
                            const float x1 = LM_GGML_FP16_TO_FP32(src[n_dims/2]);

                            dst_data[0]        = LM_GGML_FP32_TO_FP16(x0*cos_theta - x1*sin_theta);
                            dst_data[n_dims/2] = LM_GGML_FP32_TO_FP16(x0*sin_theta + x1*cos_theta);
                        }
                    }
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_rope(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_rope_f16(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_rope_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_rope_back

static void lm_ggml_compute_forward_rope_back_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // y = rope(x, src1)
    // dx = rope_back(dy, src1)
    // src0 is dy, src1 contains options

    float freq_base;
    float freq_scale;

    // these two only relevant for xPos RoPE:
    float xpos_base;
    bool xpos_down;

    //const int n_past = ((int32_t *) dst->op_params)[0];
    const int n_dims = ((int32_t *) dst->op_params)[1];
    const int mode   = ((int32_t *) dst->op_params)[2];
    const int n_ctx  = ((int32_t *) dst->op_params)[3]; UNUSED(n_ctx);
    memcpy(&freq_base,  (int32_t *) dst->op_params + 4, sizeof(float));
    memcpy(&freq_scale, (int32_t *) dst->op_params + 5, sizeof(float));
    memcpy(&xpos_base,  (int32_t *) dst->op_params + 6, sizeof(float));
    memcpy(&xpos_down,  (int32_t *) dst->op_params + 7, sizeof(bool));

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    //printf("ne0: %d, ne1: %d, ne2: %d, ne3: %d\n", ne0, ne1, ne2, ne3);
    //printf("n_past = %d, ne2 = %d\n", n_past, ne2);

    assert(nb0 == sizeof(float));

    const int ith = params->ith;
    const int nth = params->nth;

    const int nr = lm_ggml_nrows(dst);

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    // row index used to determine which thread to use
    int ir = 0;

    const float theta_scale = powf(freq_base, -2.0f/n_dims);

    const bool is_neox = mode & 2;

    const int32_t * pos = (const int32_t *) src1->data;

    for (int64_t i3 = 0; i3 < ne3; i3++) {
        for (int64_t i2 = 0; i2 < ne2; i2++) {
            const int64_t p = pos[i2];
            for (int64_t i1 = 0; i1 < ne1; i1++) {
                if (ir++ < ir0) continue;
                if (ir   > ir1) break;

                float theta = freq_scale * (float)p;

                if (!is_neox) {
                    for (int64_t i0 = 0; i0 < ne0; i0 += 2) {
                        const float cos_theta = cosf(theta);
                        const float sin_theta = sinf(theta);
                        // zeta scaling for xPos only:
                        float zeta = xpos_base != 0.0f ? powf((i0 + 0.4f * ne0) / (1.4f * ne0), p / xpos_base) : 1.0f;
                        if (xpos_down) zeta = 1.0f / zeta;

                        theta *= theta_scale;

                        const float * const dy  = (float *)((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01 + i0*nb00);
                              float *       dx  = (float *)((char *)  dst->data + i3*nb3  + i2*nb2  + i1*nb1  + i0*nb0);

                        const float dy0 = dy[0];
                        const float dy1 = dy[1];

                        dx[0] =   dy0*cos_theta*zeta + dy1*sin_theta*zeta;
                        dx[1] = - dy0*sin_theta*zeta + dy1*cos_theta*zeta;
                    }
                } else {
                    for (int64_t ib = 0; ib < ne0/n_dims; ++ib) {
                        for (int64_t ic = 0; ic < n_dims; ic += 2) {
                            const float cos_theta = cosf(theta);
                            const float sin_theta = sinf(theta);

                            theta *= theta_scale;

                            const int64_t i0 = ib*n_dims + ic/2;

                            const float * const dy  = (float *)((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01 + i0*nb00);
                                  float *       dx  = (float *)((char *)  dst->data + i3*nb3  + i2*nb2  + i1*nb1  + i0*nb0);

                            const float dy0 = dy[0];
                            const float dy1 = dy[n_dims/2];

                            dx[0]        =   dy0*cos_theta + dy1*sin_theta;
                            dx[n_dims/2] = - dy0*sin_theta + dy1*cos_theta;
                        }
                    }
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_rope_back_f16(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // y = rope(x, src1)
    // dx = rope_back(dy, src1)
    // src0 is dy, src1 contains options

    //const int n_past = ((int32_t *) dst->op_params)[0];
    const int n_dims = ((int32_t *) dst->op_params)[1];
    const int mode   = ((int32_t *) dst->op_params)[2];

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    //printf("ne0: %d, ne1: %d, ne2: %d, ne3: %d\n", ne0, ne1, ne2, ne3);
    //printf("n_past = %d, ne2 = %d\n", n_past, ne2);

    assert(nb0 == sizeof(lm_ggml_fp16_t));

    const int ith = params->ith;
    const int nth = params->nth;

    const int nr = lm_ggml_nrows(dst);

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    // row index used to determine which thread to use
    int ir = 0;

    const float theta_scale = powf(10000.0, -2.0f/n_dims);

    const bool is_neox = mode & 2;

    const int32_t * pos = (const int32_t *) src1->data;

    for (int64_t i3 = 0; i3 < ne3; i3++) {
        for (int64_t i2 = 0; i2 < ne2; i2++) {
            const int64_t p = pos[i2];
            for (int64_t i1 = 0; i1 < ne1; i1++) {
                if (ir++ < ir0) continue;
                if (ir   > ir1) break;

                float theta = (float)p;

                if (!is_neox) {
                    for (int64_t i0 = 0; i0 < ne0; i0 += 2) {
                        const float cos_theta = cosf(theta);
                        const float sin_theta = sinf(theta);

                        theta *= theta_scale;

                        const lm_ggml_fp16_t * const dy  = (lm_ggml_fp16_t *)((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01 + i0*nb00);
                              lm_ggml_fp16_t *       dx  = (lm_ggml_fp16_t *)((char *)  dst->data + i3*nb3  + i2*nb2  + i1*nb1  + i0*nb0);

                        const float dy0 = LM_GGML_FP16_TO_FP32(dy[0]);
                        const float dy1 = LM_GGML_FP16_TO_FP32(dy[1]);

                        dx[0] = LM_GGML_FP32_TO_FP16( dy0*cos_theta + dy1*sin_theta);
                        dx[1] = LM_GGML_FP32_TO_FP16(-dy0*sin_theta + dy1*cos_theta);
                    }
                } else {
                    for (int64_t ib = 0; ib < ne0/n_dims; ++ib) {
                        for (int64_t ic = 0; ic < n_dims; ic += 2) {
                            const float cos_theta = cosf(theta);
                            const float sin_theta = sinf(theta);

                            theta *= theta_scale;

                            const int64_t i0 = ib*n_dims + ic/2;

                            const lm_ggml_fp16_t * const dy  = (lm_ggml_fp16_t *)((char *) src0->data + i3*nb03 + i2*nb02 + i1*nb01 + i0*nb00);
                                  lm_ggml_fp16_t *       dx  = (lm_ggml_fp16_t *)((char *)  dst->data + i3*nb3  + i2*nb2  + i1*nb1  + i0*nb0);

                            const float dy0 = LM_GGML_FP16_TO_FP32(dy[0]);
                            const float dy1 = LM_GGML_FP16_TO_FP32(dy[n_dims/2]);

                            dx[0]        = LM_GGML_FP32_TO_FP16( dy0*cos_theta + dy1*sin_theta);
                            dx[n_dims/2] = LM_GGML_FP32_TO_FP16(-dy0*sin_theta + dy1*cos_theta);
                        }
                    }
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_rope_back(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_rope_back_f16(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_rope_back_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_conv_1d

static void lm_ggml_compute_forward_conv_1d_f16_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(src0->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT( dst->type == LM_GGML_TYPE_F32);

    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    const int ith = params->ith;
    const int nth = params->nth;

    const int nk = ne00;

    // size of the convolution row - the kernel size unrolled across all input channels
    const int ew0 = nk*ne01;

    const int32_t s0 = ((const int32_t*)(dst->op_params))[0];
    const int32_t p0 = ((const int32_t*)(dst->op_params))[1];
    const int32_t d0 = ((const int32_t*)(dst->op_params))[2];

    LM_GGML_ASSERT(nb00 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb10 == sizeof(float));

    if (params->type == LM_GGML_TASK_INIT) {
        memset(params->wdata, 0, params->wsize);

        lm_ggml_fp16_t * const wdata = (lm_ggml_fp16_t *) params->wdata + 0;

        for (int64_t i11 = 0; i11 < ne11; i11++) {
            const float * const src = (float *)((char *) src1->data + i11*nb11);
            lm_ggml_fp16_t * dst_data = wdata;

            for (int64_t i0 = 0; i0 < ne0; i0++) {
                for (int64_t ik = 0; ik < nk; ik++) {
                    const int idx0 = i0*s0 + ik*d0 - p0;

                    if(!(idx0 < 0 || idx0 >= ne10)) {
                        dst_data[i0*ew0 + i11*nk + ik] = LM_GGML_FP32_TO_FP16(src[idx0]);
                    }
                }
            }
        }

        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // total rows in dst
    const int nr = ne2;

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    lm_ggml_fp16_t * const wdata = (lm_ggml_fp16_t *) params->wdata + 0;

    for (int i2 = 0; i2 < ne2; i2++) {
        for (int i1 = ir0; i1 < ir1; i1++) {
            float * dst_data = (float *)((char *) dst->data + i2*nb2 + i1*nb1);

            for (int i0 = 0; i0 < ne0; i0++) {
                lm_ggml_vec_dot_f16(ew0, dst_data + i0,
                        (lm_ggml_fp16_t *) ((char *) src0->data + i1*nb02),
                        (lm_ggml_fp16_t *)                wdata + i2*nb2 + i0*ew0);
            }
        }
    }
}

static void lm_ggml_compute_forward_conv_1d_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(src0->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT( dst->type == LM_GGML_TYPE_F32);

    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    const int ith = params->ith;
    const int nth = params->nth;

    const int nk = ne00;

    const int ew0 = nk*ne01;

    const int32_t s0 = ((const int32_t*)(dst->op_params))[0];
    const int32_t p0 = ((const int32_t*)(dst->op_params))[1];
    const int32_t d0 = ((const int32_t*)(dst->op_params))[2];

    LM_GGML_ASSERT(nb00 == sizeof(float));
    LM_GGML_ASSERT(nb10 == sizeof(float));

    if (params->type == LM_GGML_TASK_INIT) {
        memset(params->wdata, 0, params->wsize);

        float * const wdata = (float *) params->wdata + 0;

        for (int64_t i11 = 0; i11 < ne11; i11++) {
            const float * const src = (float *)((char *) src1->data + i11*nb11);
            float * dst_data = wdata;

            for (int64_t i0 = 0; i0 < ne0; i0++) {
                for (int64_t ik = 0; ik < nk; ik++) {
                    const int idx0 = i0*s0 + ik*d0 - p0;

                    if(!(idx0 < 0 || idx0 >= ne10)) {
                        dst_data[i0*ew0 + i11*nk + ik] = src[idx0];
                    }
                }
            }
        }

        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // total rows in dst
    const int nr = ne02;

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    float * const wdata = (float *) params->wdata + 0;

    for (int i2 = 0; i2 < ne2; i2++) {
        for (int i1 = ir0; i1 < ir1; i1++) {
            float * dst_data = (float *)((char *) dst->data + i2*nb2 + i1*nb1);

            for (int i0 = 0; i0 < ne0; i0++) {
                lm_ggml_vec_dot_f32(ew0, dst_data + i0,
                        (float *) ((char *) src0->data + i1*nb02),
                        (float *)                wdata + i2*nb2 + i0*ew0);
            }
        }
    }
}

// TODO: reuse lm_ggml_mul_mat or implement lm_ggml_im2col and remove stage_0 and stage_1
static void gemm_f16_out_f32(int64_t m, int64_t n, int64_t k,
                             lm_ggml_fp16_t * A,
                             lm_ggml_fp16_t * B,
                             float * C,
                             const int ith, const int nth) {
    // does not seem to make a difference
    int64_t m0, m1, n0, n1;
    // patches per thread
    if (m > n) {
        n0 = 0;
        n1 = n;

        // total patches in dst
        const int np = m;

        // patches per thread
        const int dp = (np + nth - 1)/nth;

        // patch range for this thread
        m0 = dp*ith;
        m1 = MIN(m0 + dp, np);
    } else {
        m0 = 0;
        m1 = m;

        // total patches in dst
        const int np = n;

        // patches per thread
        const int dp = (np + nth - 1)/nth;

        // patch range for this thread
        n0 = dp*ith;
        n1 = MIN(n0 + dp, np);
    }

    // block-tiling attempt
    int64_t blck_n = 16;
    int64_t blck_m = 16;

    // int64_t CACHE_SIZE = 2 * 1024 * 1024; // 2MB
    // int64_t blck_size = CACHE_SIZE / (sizeof(float) + 2 * sizeof(lm_ggml_fp16_t) * K);
    // if (blck_size > 0) {
    //     blck_0 = 4;
    //     blck_1 = blck_size / blck_0;
    //     if (blck_1 < 0) {
    //         blck_1 = 1;
    //     }
    //     // blck_0 = (int64_t)sqrt(blck_size);
    //     // blck_1 = blck_0;
    // }
    // // printf("%zd %zd %zd %zd\n", blck_size, K, blck_0, blck_1);

    for (int j = n0; j < n1; j+=blck_n) {
        for (int i = m0; i < m1; i+=blck_m) {
            // printf("i j k => %d %d %d\n", i, j, K);
            for (int ii = i; ii < i + blck_m && ii < m1; ii++) {
                for (int jj = j; jj < j + blck_n && jj < n1; jj++) {
                    lm_ggml_vec_dot_f16(k,
                                    C + ii*n + jj,
                                    A + ii * k,
                                    B + jj * k);
                }
            }
        }
    }
}

// src0: kernel [OC, IC, K]
// src1: signal [N, IC, IL]
// dst:  result [N, OL, IC*K]
static void lm_ggml_compute_forward_conv_1d_stage_0_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(src0->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT( dst->type == LM_GGML_TYPE_F16);

    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS;

    const int64_t N  = ne12;
    const int64_t IC = ne11;
    const int64_t IL = ne10;

    const int64_t K = ne00;

    const int64_t OL = ne1;

    const int ith = params->ith;
    const int nth = params->nth;

    const int32_t s0 = ((const int32_t*)(dst->op_params))[0];
    const int32_t p0 = ((const int32_t*)(dst->op_params))[1];
    const int32_t d0 = ((const int32_t*)(dst->op_params))[2];

    LM_GGML_ASSERT(nb00 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb10 == sizeof(float));

    if (params->type == LM_GGML_TASK_INIT) {
        memset(dst->data, 0, lm_ggml_nbytes(dst));
        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // im2col: [N, IC, IL] => [N, OL, IC*K]
    {
        lm_ggml_fp16_t * const wdata = (lm_ggml_fp16_t *) dst->data;

        for (int64_t in = 0; in < N; in++) {
            for (int64_t iol = 0; iol < OL; iol++) {
                for (int64_t iic = ith; iic < IC; iic+=nth) {

                    // micro kernel
                    lm_ggml_fp16_t * dst_data = wdata + (in*OL + iol)*(IC*K); // [IC, K]
                    const float * const src_data = (float *)((char *) src1->data + in*nb12 + iic*nb11); // [IL]

                    for (int64_t ik = 0; ik < K; ik++) {
                        const int64_t iil = iol*s0 + ik*d0 - p0;

                        if (!(iil < 0 || iil >= IL)) {
                            dst_data[iic*K + ik] = LM_GGML_FP32_TO_FP16(src_data[iil]);
                        }
                    }
                }
            }
        }
    }
}

// gemm: [N, OC, OL] = [OC, IC * K] x [N*OL, IC * K]
// src0: [OC, IC, K]
// src1: [N, OL, IC * K]
// result: [N, OC, OL]
static void lm_ggml_compute_forward_conv_1d_stage_1_f16(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(src0->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT( dst->type == LM_GGML_TYPE_F32);

    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    if (params->type == LM_GGML_TASK_INIT) {
        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_TENSOR_BINARY_OP_LOCALS;

    LM_GGML_ASSERT(nb00 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb10 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb0  == sizeof(float));

    const int N = ne12;
    const int OL = ne11;

    const int OC = ne02;
    const int IC = ne01;
    const int K  = ne00;

    const int ith = params->ith;
    const int nth = params->nth;

    int64_t m = OC;
    int64_t n = OL;
    int64_t k = IC * K;

    // [N, OC, OL] = [OC, IC * K] x [N*OL, IC * K]
    for (int i = 0; i < N; i++) {
        lm_ggml_fp16_t * A = (lm_ggml_fp16_t *)src0->data; // [m, k]
        lm_ggml_fp16_t * B = (lm_ggml_fp16_t *)src1->data + i * m * k; // [n, k]
        float * C = (float *)dst->data + i * m * n; // [m, n]

        gemm_f16_out_f32(m, n, k, A, B, C, ith, nth);
    }
}

static void lm_ggml_compute_forward_conv_1d(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    switch(src0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_conv_1d_f16_f32(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_conv_1d_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

static void lm_ggml_compute_forward_conv_1d_stage_0(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    switch(src0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_conv_1d_stage_0_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

static void lm_ggml_compute_forward_conv_1d_stage_1(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    switch(src0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_conv_1d_stage_1_f16(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_conv_transpose_1d

static void lm_ggml_compute_forward_conv_transpose_1d_f16_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(src0->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT( dst->type == LM_GGML_TYPE_F32);

    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    const int ith = params->ith;
    const int nth = params->nth;

    const int nk = ne00*ne01*ne02;

    LM_GGML_ASSERT(nb00 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb10 == sizeof(float));

    if (params->type == LM_GGML_TASK_INIT) {
        memset(params->wdata, 0, params->wsize);

        // permute kernel data (src0) from (K x Cout x Cin) to (Cin x K x Cout)
        {
            lm_ggml_fp16_t * const wdata = (lm_ggml_fp16_t *) params->wdata + 0;

            for (int64_t i02 = 0; i02 < ne02; i02++) {
                for (int64_t i01 = 0; i01 < ne01; i01++) {
                    const lm_ggml_fp16_t * const src = (lm_ggml_fp16_t *)((char *) src0->data + i02*nb02 + i01*nb01);
                    lm_ggml_fp16_t * dst_data = wdata + i01*ne00*ne02;
                    for (int64_t i00 = 0; i00 < ne00; i00++) {
                        dst_data[i00*ne02 + i02] = src[i00];
                    }
                }
            }
        }

        // permute source data (src1) from (L x Cin) to (Cin x L)
        {
            lm_ggml_fp16_t * const wdata = (lm_ggml_fp16_t *) params->wdata + nk;
            lm_ggml_fp16_t * dst_data = wdata;

            for (int64_t i11 = 0; i11 < ne11; i11++) {
                const float * const src = (float *)((char *) src1->data + i11*nb11);
                for (int64_t i10 = 0; i10 < ne10; i10++) {
                    dst_data[i10*ne11 + i11] = LM_GGML_FP32_TO_FP16(src[i10]);
                }
            }
        }

        // need to zero dst since we are accumulating into it
        memset(dst->data, 0, lm_ggml_nbytes(dst));

        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int32_t s0 = ((const int32_t*)(dst->op_params))[0];

    // total rows in dst
    const int nr = ne1;

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    lm_ggml_fp16_t * const wdata     = (lm_ggml_fp16_t *) params->wdata + 0;
    lm_ggml_fp16_t * const wdata_src = wdata + nk;

    for (int i1 = ir0; i1 < ir1; i1++) {
        float * dst_data = (float *)((char *) dst->data + i1*nb1);
        lm_ggml_fp16_t * wdata_kernel = wdata + i1*ne02*ne00;
        for (int i10 = 0; i10 < ne10; i10++) {
            const int i1n = i10*ne11;
            for (int i00 = 0; i00 < ne00; i00++) {
                float v = 0;
                lm_ggml_vec_dot_f16(ne02, &v,
                        (lm_ggml_fp16_t *)    wdata_src + i1n,
                        (lm_ggml_fp16_t *) wdata_kernel + i00*ne02);
                dst_data[i10*s0 + i00] += v;
            }
        }
    }
}

static void lm_ggml_compute_forward_conv_transpose_1d_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(src0->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT( dst->type == LM_GGML_TYPE_F32);

    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    const int ith = params->ith;
    const int nth = params->nth;

    const int nk = ne00*ne01*ne02;

    LM_GGML_ASSERT(nb00 == sizeof(float));
    LM_GGML_ASSERT(nb10 == sizeof(float));

    if (params->type == LM_GGML_TASK_INIT) {
        memset(params->wdata, 0, params->wsize);

        // prepare kernel data (src0) from (K x Cout x Cin) to (Cin x K x Cout)
        {
            float * const wdata = (float *) params->wdata + 0;

            for (int64_t i02 = 0; i02 < ne02; i02++) {
                for (int64_t i01 = 0; i01 < ne01; i01++) {
                    const float * const src = (float *)((char *) src0->data + i02*nb02 + i01*nb01);
                    float * dst_data = wdata + i01*ne00*ne02;
                    for (int64_t i00 = 0; i00 < ne00; i00++) {
                        dst_data[i00*ne02 + i02] = src[i00];
                    }
                }
            }
        }

        // prepare source data (src1)
        {
            float * const wdata = (float *) params->wdata + nk;
            float * dst_data = wdata;

            for (int64_t i11 = 0; i11 < ne11; i11++) {
                const float * const src = (float *)((char *) src1->data + i11*nb11);
                for (int64_t i10 = 0; i10 < ne10; i10++) {
                    dst_data[i10*ne11 + i11] = src[i10];
                }
            }
        }

        // need to zero dst since we are accumulating into it
        memset(dst->data, 0, lm_ggml_nbytes(dst));

        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int32_t s0 = ((const int32_t*)(dst->op_params))[0];

    // total rows in dst
    const int nr = ne1;

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    float * const wdata     = (float *) params->wdata + 0;
    float * const wdata_src = wdata + nk;

    for (int i1 = ir0; i1 < ir1; i1++) {
        float * dst_data = (float *)((char *) dst->data + i1*nb1);
        float * wdata_kernel = wdata + i1*ne02*ne00;
        for (int i10 = 0; i10 < ne10; i10++) {
            const int i1n = i10*ne11;
            for (int i00 = 0; i00 < ne00; i00++) {
                float v = 0;
                lm_ggml_vec_dot_f32(ne02, &v,
                        wdata_src + i1n,
                        wdata_kernel + i00*ne02);
                dst_data[i10*s0 + i00] += v;
            }
        }
    }
}

static void lm_ggml_compute_forward_conv_transpose_1d(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_conv_transpose_1d_f16_f32(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_conv_transpose_1d_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_conv_2d

// src0: kernel [OC, IC, KH, KW]
// src1: image [N, IC, IH, IW]
// dst:  result [N, OH, OW, IC*KH*KW]
static void lm_ggml_compute_forward_conv_2d_stage_0_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(src0->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT( dst->type == LM_GGML_TYPE_F16);

    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS;

    const int64_t N = ne13;
    const int64_t IC = ne12;
    const int64_t IH = ne11;
    const int64_t IW = ne10;

    // const int64_t OC = ne03;
    // const int64_t IC = ne02;
    const int64_t KH = ne01;
    const int64_t KW = ne00;

    const int64_t OH = ne2;
    const int64_t OW = ne1;

    const int ith = params->ith;
    const int nth = params->nth;

    const int32_t s0 = ((const int32_t*)(dst->op_params))[0];
    const int32_t s1 = ((const int32_t*)(dst->op_params))[1];
    const int32_t p0 = ((const int32_t*)(dst->op_params))[2];
    const int32_t p1 = ((const int32_t*)(dst->op_params))[3];
    const int32_t d0 = ((const int32_t*)(dst->op_params))[4];
    const int32_t d1 = ((const int32_t*)(dst->op_params))[5];

    LM_GGML_ASSERT(nb00 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb10 == sizeof(float));

    if (params->type == LM_GGML_TASK_INIT) {
        memset(dst->data, 0, lm_ggml_nbytes(dst));
        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // im2col: [N, IC, IH, IW] => [N, OH, OW, IC*KH*KW]
    {
        lm_ggml_fp16_t * const wdata = (lm_ggml_fp16_t *) dst->data;

        for (int64_t in = 0; in < N; in++) {
            for (int64_t ioh = 0; ioh < OH; ioh++) {
                for (int64_t iow = 0; iow < OW; iow++) {
                    for (int64_t iic = ith; iic < IC; iic+=nth) {

                        // micro kernel
                        lm_ggml_fp16_t * dst_data = wdata + (in*OH*OW + ioh*OW + iow)*(IC*KH*KW); // [IC, KH, KW]
                        const float * const src_data = (float *)((char *) src1->data + in*nb13 + iic*nb12); // [IH, IW]

                        for (int64_t ikh = 0; ikh < KH; ikh++) {
                            for (int64_t ikw = 0; ikw < KW; ikw++) {
                                const int64_t iiw = iow*s0 + ikw*d0 - p0;
                                const int64_t iih = ioh*s1 + ikh*d1 - p1;

                                if (!(iih < 0 || iih >= IH || iiw < 0 || iiw >= IW)) {
                                    dst_data[iic*(KH*KW) + ikh*KW + ikw] = LM_GGML_FP32_TO_FP16(src_data[iih*IW + iiw]);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// gemm: [N, OC, OH, OW] = [OC, IC * KH * KW] x [N*OH*OW, IC * KH * KW]
// src0: [OC, IC, KH, KW]
// src1: [N, OH, OW, IC * KH * KW]
// result: [N, OC, OH, OW]
static void lm_ggml_compute_forward_conv_2d_stage_1_f16(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(src0->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT( dst->type == LM_GGML_TYPE_F32);

    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    if (params->type == LM_GGML_TASK_INIT) {
        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_TENSOR_BINARY_OP_LOCALS;

    LM_GGML_ASSERT(nb00 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb10 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb0  == sizeof(float));

    const int N = ne13;
    const int OH = ne12;
    const int OW = ne11;

    const int OC = ne03;
    const int IC = ne02;
    const int KH = ne01;
    const int KW = ne00;

    const int ith = params->ith;
    const int nth = params->nth;

    int64_t m = OC;
    int64_t n = OH * OW;
    int64_t k = IC * KH * KW;

    // [N, OC, OH, OW] = [OC, IC * KH * KW] x [N*OH*OW, IC * KH * KW]
    for (int i = 0; i < N; i++) {
        lm_ggml_fp16_t * A = (lm_ggml_fp16_t *)src0->data; // [m, k]
        lm_ggml_fp16_t * B = (lm_ggml_fp16_t *)src1->data + i * m * k; // [n, k]
        float * C = (float *)dst->data + i * m * n; // [m, n]

        gemm_f16_out_f32(m, n, k, A, B, C, ith, nth);
    }
}

static void lm_ggml_compute_forward_conv_2d_f16_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(src0->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT( dst->type == LM_GGML_TYPE_F32);

    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    // src1: image [N, IC, IH, IW]
    // src0: kernel [OC, IC, KH, KW]
    // dst:  result [N, OC, OH, OW]
    // ne12: IC
    // ne0: OW
    // ne1: OH
    // nk0: KW
    // nk1: KH
    // ne13: N

    const int N = ne13;
    const int IC = ne12;
    const int IH = ne11;
    const int IW = ne10;

    const int OC = ne03;
    // const int IC = ne02;
    const int KH = ne01;
    const int KW = ne00;

    const int OH = ne1;
    const int OW = ne0;

    const int ith = params->ith;
    const int nth = params->nth;

    // const int nk0 = ne00;
    // const int nk1 = ne01;

    // size of the convolution row - the kernel size unrolled across all channels
    // const int ew0 = nk0*nk1*ne02;
    // ew0: IC*KH*KW

    const int32_t s0 = ((const int32_t*)(dst->op_params))[0];
    const int32_t s1 = ((const int32_t*)(dst->op_params))[1];
    const int32_t p0 = ((const int32_t*)(dst->op_params))[2];
    const int32_t p1 = ((const int32_t*)(dst->op_params))[3];
    const int32_t d0 = ((const int32_t*)(dst->op_params))[4];
    const int32_t d1 = ((const int32_t*)(dst->op_params))[5];

    LM_GGML_ASSERT(nb00 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb10 == sizeof(float));

    if (params->type == LM_GGML_TASK_INIT) {
        memset(params->wdata, 0, params->wsize);

        // prepare source data (src1)
        // im2col: [N, IC, IH, IW] => [N*OH*OW, IC*KH*KW]

        {
            lm_ggml_fp16_t * const wdata = (lm_ggml_fp16_t *) params->wdata + 0;

            for (int in = 0; in < N; in++) {
                for (int iic = 0; iic < IC; iic++) {
                    for (int ioh = 0; ioh < OH; ioh++) {
                        for (int iow = 0; iow < OW; iow++) {

                            // micro kernel
                            lm_ggml_fp16_t * dst_data = wdata + (in*OH*OW + ioh*OW + iow)*(IC*KH*KW); // [IC, KH, KW]
                            const float * const src_data = (float *)((char *) src1->data + in*nb13 + iic*nb12); // [IH, IW]

                            for (int ikh = 0; ikh < KH; ikh++) {
                                for (int ikw = 0; ikw < KW; ikw++) {
                                    const int iiw = iow*s0 + ikw*d0 - p0;
                                    const int iih = ioh*s1 + ikh*d1 - p1;

                                    if (!(iih < 0 || iih >= IH || iiw < 0 || iiw >= IW)) {
                                        dst_data[iic*(KH*KW) + ikh*KW + ikw] = LM_GGML_FP32_TO_FP16(src_data[iih*IW + iiw]);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    lm_ggml_fp16_t * const wdata = (lm_ggml_fp16_t *) params->wdata + 0;
    // wdata: [N*OH*OW, IC*KH*KW]
    // dst: result [N, OC, OH, OW]
    // src0: kernel [OC, IC, KH, KW]

    int64_t m = OC;
    int64_t n = OH * OW;
    int64_t k = IC * KH * KW;

    // [N, OC, OH, OW] = [OC, IC * KH * KW] x [N*OH*OW, IC * KH * KW]
    for (int i = 0; i < N; i++) {
        lm_ggml_fp16_t * A = (lm_ggml_fp16_t *)src0->data; // [m, k]
        lm_ggml_fp16_t * B = (lm_ggml_fp16_t *)wdata + i * m * k; // [n, k]
        float * C = (float *)dst->data + i * m * n; // [m * k]

        gemm_f16_out_f32(m, n, k, A, B, C, ith, nth);
    }
}

static void lm_ggml_compute_forward_conv_2d(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_conv_2d_f16_f32(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                //lm_ggml_compute_forward_conv_2d_f32(params, src0, src1, dst);
                LM_GGML_ASSERT(false);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

static void lm_ggml_compute_forward_conv_2d_stage_0(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_conv_2d_stage_0_f32(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                LM_GGML_ASSERT(false);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

static void lm_ggml_compute_forward_conv_2d_stage_1(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_conv_2d_stage_1_f16(params, src0, src1, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                LM_GGML_ASSERT(false);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_conv_transpose_2d

static void lm_ggml_compute_forward_conv_transpose_2d(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
              struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(src0->type == LM_GGML_TYPE_F16);
    LM_GGML_ASSERT(src1->type == LM_GGML_TYPE_F32);
    LM_GGML_ASSERT( dst->type == LM_GGML_TYPE_F32);

    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    LM_GGML_TENSOR_BINARY_OP_LOCALS

    const int ith = params->ith;
    const int nth = params->nth;

    const int nk = ne00*ne01*ne02*ne03;

    LM_GGML_ASSERT(nb00 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nb10 == sizeof(float));

    if (params->type == LM_GGML_TASK_INIT) {
        memset(params->wdata, 0, params->wsize);

        // permute kernel data (src0) from (Kw x Kh x Cout x Cin) to (Cin x Kw x Kh x Cout)
        {
            lm_ggml_fp16_t * const wdata = (lm_ggml_fp16_t *) params->wdata + 0;

            for (int64_t i03 = 0; i03 < ne03; i03++) {
                for (int64_t i02 = 0; i02 < ne02; i02++) {
                    const lm_ggml_fp16_t * const src = (lm_ggml_fp16_t *)((char *) src0->data + i03*nb03 + i02*nb02);
                    lm_ggml_fp16_t * dst_data = wdata + i02*ne01*ne00*ne03;
                    for (int64_t i01 = 0; i01 < ne01; i01++) {
                        for (int64_t i00 = 0; i00 < ne00; i00++) {
                            dst_data[i01*ne00*ne03 + i00*ne03 + i03] = src[i01 * ne00 + i00];
                        }
                    }
                }
            }
        }

        // permute source data (src1) from (Sw x Sh x Cin) to (Cin x Sw x Sh)
        {
            lm_ggml_fp16_t * const wdata = (lm_ggml_fp16_t *) params->wdata + nk;
            for (int i12 = 0; i12 < ne12; i12++) {
                for (int i11 = 0; i11 < ne11; i11++) {
                    const float * const src = (float *)((char *) src1->data + i12*nb12 + i11*nb11);
                    lm_ggml_fp16_t * dst_data = wdata + i11*ne10*ne12;
                    for (int i10 = 0; i10 < ne10; i10++) {
                        dst_data[i10*ne12 + i12] = LM_GGML_FP32_TO_FP16(src[i10]);
                    }
                }
            }
        }

        memset(dst->data, 0, lm_ggml_nbytes(dst));

        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int32_t stride = lm_ggml_get_op_params_i32(dst, 0);

    // total patches in dst
    const int np = ne2;

    // patches per thread
    const int dp = (np + nth - 1)/nth;

    // patch range for this thread
    const int ip0 = dp*ith;
    const int ip1 = MIN(ip0 + dp, np);

    lm_ggml_fp16_t * const wdata = (lm_ggml_fp16_t *) params->wdata + 0;
    lm_ggml_fp16_t * const wdata_src = wdata + nk;

    for (int i2 = ip0; i2 < ip1; i2++) { // Cout
        float * dst_data = (float *)((char *) dst->data + i2*nb2);
        lm_ggml_fp16_t * wdata_kernel = wdata + i2*ne01*ne00*ne03;
        for (int i11 = 0; i11 < ne11; i11++) {
            for (int i10 = 0; i10 < ne10; i10++) {
                const int i1n = i11*ne10*ne12 + i10*ne12;
                for (int i01 = 0; i01 < ne01; i01++) {
                    for (int i00 = 0; i00 < ne00; i00++) {
                        float v = 0;
                        lm_ggml_vec_dot_f16(ne03, &v,
                                wdata_src + i1n,
                                wdata_kernel + i01*ne00*ne03 + i00*ne03);
                        dst_data[(i11*stride + i01)*ne0 + i10*stride + i00] += v;
                    }
                }
            }
        }
    }
}

// lm_ggml_compute_forward_pool_1d_sk_p0

static void lm_ggml_compute_forward_pool_1d_sk_p0(
        const struct lm_ggml_compute_params * params,
        const enum lm_ggml_op_pool op,
        const struct lm_ggml_tensor * src,
        const int k,
        struct lm_ggml_tensor * dst) {
    assert(src->type == LM_GGML_TYPE_F32);
    assert(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const char * cdata = (const char *)src->data;
    const char * const data_end = cdata + lm_ggml_nbytes(src);
    float * drow = (float *)dst->data;

    const int64_t rs = dst->ne[0];

    while (cdata < data_end) {
        const float * const srow = (const float *)cdata;

        int j = 0;

        for (int64_t i = 0; i < rs; ++i) {
            switch (op) {
                case LM_GGML_OP_POOL_AVG:   drow[i] = 0;        break;
                case LM_GGML_OP_POOL_MAX:   drow[i] = -FLT_MAX; break;
                case LM_GGML_OP_POOL_COUNT: LM_GGML_ASSERT(false); break;
            }
            for (int ki = 0; ki < k; ++ki) {
                switch (op) {
                    case LM_GGML_OP_POOL_AVG:                          drow[i] += srow[j]; break;
                    case LM_GGML_OP_POOL_MAX:   if (srow[j] > drow[i]) drow[i]  = srow[j]; break;
                    case LM_GGML_OP_POOL_COUNT:                        LM_GGML_ASSERT(false); break;
                }
                ++j;
            }
            switch (op) {
                case LM_GGML_OP_POOL_AVG:         drow[i] /= k; break;
                case LM_GGML_OP_POOL_MAX:                       break;
                case LM_GGML_OP_POOL_COUNT: LM_GGML_ASSERT(false); break;
            }
        }

        cdata += src->nb[1];
        drow  += rs;
    }
}

// lm_ggml_compute_forward_pool_1d

static void lm_ggml_compute_forward_pool_1d(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
              struct lm_ggml_tensor * dst) {

    const int32_t * opts = (const int32_t *)dst->op_params;
    enum lm_ggml_op_pool op = opts[0];
    const int k0 = opts[1];
    const int s0 = opts[2];
    const int p0 = opts[3];
    LM_GGML_ASSERT(p0 == 0); // padding not supported
    LM_GGML_ASSERT(k0 == s0); // only s = k supported

    lm_ggml_compute_forward_pool_1d_sk_p0(params, op, src0, k0, dst);
}

// lm_ggml_compute_forward_pool_2d_sk_p0

static void lm_ggml_compute_forward_pool_2d_sk_p0(
        const struct lm_ggml_compute_params * params,
        const enum   lm_ggml_op_pool op,
        const struct lm_ggml_tensor * src,
        const int k0,
        const int k1,
        struct lm_ggml_tensor * dst) {
    assert(src->type == LM_GGML_TYPE_F32);
    assert(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const char * cdata = (const char*)src->data;
    const char * const data_end = cdata + lm_ggml_nbytes(src);

    const int64_t px = dst->ne[0];
    const int64_t py = dst->ne[1];
    const int64_t pa = px * py;

    float * dplane = (float *)dst->data;

    const int ka = k0 * k1;

    while (cdata < data_end) {
        for (int oy = 0; oy < py; ++oy) {
            float * const drow = dplane + oy * px;
            for (int ox = 0; ox < px; ++ox) {
                float * const out =  drow + ox;
                switch (op) {
                    case LM_GGML_OP_POOL_AVG:     *out = 0;        break;
                    case LM_GGML_OP_POOL_MAX:     *out = -FLT_MAX; break;
                    case LM_GGML_OP_POOL_COUNT: LM_GGML_ASSERT(false); break;
                }

                const int ix = ox * k0;
                const int iy = oy * k1;

                for (int ky = 0; ky < k1; ++ky) {
                    const float * const srow = (const float *)(cdata + src->nb[1] * (iy + ky));
                    for (int kx = 0; kx < k0; ++kx) {
                        int j = ix + kx;
                        switch (op) {
                            case LM_GGML_OP_POOL_AVG:                     *out += srow[j]; break;
                            case LM_GGML_OP_POOL_MAX: if (srow[j] > *out) *out  = srow[j]; break;
                            case LM_GGML_OP_POOL_COUNT:                LM_GGML_ASSERT(false); break;
                        }
                    }
                }
                switch (op) {
                    case LM_GGML_OP_POOL_AVG:           *out /= ka; break;
                    case LM_GGML_OP_POOL_MAX:                       break;
                    case LM_GGML_OP_POOL_COUNT: LM_GGML_ASSERT(false); break;
                }
            }
        }

        cdata  += src->nb[2];
        dplane += pa;
    }
}

// lm_ggml_compute_forward_pool_2d

static void lm_ggml_compute_forward_pool_2d(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
              struct lm_ggml_tensor * dst) {

    const int32_t * opts = (const int32_t *)dst->op_params;
    enum lm_ggml_op_pool op = opts[0];
    const int k0 = opts[1];
    const int k1 = opts[2];
    const int s0 = opts[3];
    const int s1 = opts[4];
    const int p0 = opts[5];
    const int p1 = opts[6];
    LM_GGML_ASSERT(p0 == 0);
    LM_GGML_ASSERT(p1 == 0); // padding not supported
    LM_GGML_ASSERT(k0 == s0);
    LM_GGML_ASSERT(k1 == s1); // only s = k supported

    lm_ggml_compute_forward_pool_2d_sk_p0(params, op, src0, k0, k1, dst);
}

// lm_ggml_compute_forward_upscale

static void lm_ggml_compute_forward_upscale_f32(
    const struct lm_ggml_compute_params * params,
    const struct lm_ggml_tensor * src0,
    struct lm_ggml_tensor * dst) {

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_ASSERT(src0->nb[0] == sizeof(float));

    const int ith = params->ith;

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    const int scale_factor = dst->op_params[0];

    // TODO: optimize

    for (int i03 = 0; i03 < ne03; i03++) {
        for (int i02 = ith; i02 < ne02; i02++) {
            for (int m = 0; m < dst->ne[1]; m++) {
                int i01 = m / scale_factor;
                for (int n = 0; n < dst->ne[0]; n++) {
                    int i00 = n / scale_factor;

                    const float * x = (float *)((char *) src0->data + i00 * nb00 +i01 * nb01 + i02 * nb02 + i03 * nb03);

                    float * y = (float *)((char *) dst->data + n * dst->nb[0] + m * dst->nb[1] + i02 * dst->nb[2] + i03 * dst->nb[3]);

                    *y = *x;
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_upscale(
    const struct lm_ggml_compute_params * params,
    const struct lm_ggml_tensor * src0,
    struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_upscale_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_flash_attn

static void lm_ggml_compute_forward_flash_attn_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * q,
        const struct lm_ggml_tensor * k,
        const struct lm_ggml_tensor * v,
        const bool masked,
        struct lm_ggml_tensor * dst) {
    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    LM_GGML_TENSOR_LOCALS(int64_t, neq, q,   ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nbq, q,   nb)
    LM_GGML_TENSOR_LOCALS(int64_t, nek, k,   ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nbk, k,   nb)
    LM_GGML_TENSOR_LOCALS(int64_t, nev, v,   ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nbv, v,   nb)
    LM_GGML_TENSOR_LOCALS(int64_t, ne,  dst, ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nb,  dst, nb)

    const int ith = params->ith;
    const int nth = params->nth;

    const int64_t D = neq0;
    const int64_t N = neq1;
    const int64_t P = nek1 - N;
    const int64_t M = P + N;

    const int Mup = lm_ggml_up(M, LM_GGML_SOFT_MAX_UNROLL);

    LM_GGML_ASSERT(ne0 == D);
    LM_GGML_ASSERT(ne1 == N);
    LM_GGML_ASSERT(P >= 0);

    LM_GGML_ASSERT(nbq0 == sizeof(float));
    LM_GGML_ASSERT(nbk0 == sizeof(float));
    LM_GGML_ASSERT(nbv0 == sizeof(float));

    LM_GGML_ASSERT(neq0 == D);
    LM_GGML_ASSERT(nek0 == D);
    LM_GGML_ASSERT(nev1 == D);

    LM_GGML_ASSERT(neq1 == N);
    LM_GGML_ASSERT(nek1 == N + P);
    LM_GGML_ASSERT(nev1 == D);

    // dst cannot be transposed or permuted
    LM_GGML_ASSERT(nb0 == sizeof(float));
    LM_GGML_ASSERT(nb0 <= nb1);
    LM_GGML_ASSERT(nb1 <= nb2);
    LM_GGML_ASSERT(nb2 <= nb3);

    if (params->type == LM_GGML_TASK_INIT) {
        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // parallelize by q rows using lm_ggml_vec_dot_f32

    // total rows in q
    const int nr = neq1*neq2*neq3;

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    const float scale = 1.0f/sqrtf(D);

    //printf("P=%d N=%d D=%d ir0=%d ir1=%d scale = %f\n", P, N, D, ir0, ir1, scale);

    for (int ir = ir0; ir < ir1; ++ir) {
        // q indices
        const int iq3 = ir/(neq2*neq1);
        const int iq2 = (ir - iq3*neq2*neq1)/neq1;
        const int iq1 = (ir - iq3*neq2*neq1 - iq2*neq1);

        float * S = (float *) params->wdata + ith*(Mup + CACHE_LINE_SIZE_F32);

        for (int i = M; i < Mup; ++i) {
            S[i] = -INFINITY;
        }

        const int64_t masked_begin = masked ? (P + iq1 + 1) : M;
        for (int64_t ic = 0; ic < masked_begin; ++ic) {
            // k indices
            const int ik3 = iq3;
            const int ik2 = iq2 % nek2;
            const int ik1 = ic;

            // S indices
            const int i1 = ik1;

            lm_ggml_vec_dot_f32(neq0,
                    S + i1,
                    (float *) ((char *) k->data + (ik1*nbk1 + ik2*nbk2 + ik3*nbk3)),
                    (float *) ((char *) q->data + (iq1*nbq1 + iq2*nbq2 + iq3*nbq3)));
        }

        // scale
        lm_ggml_vec_scale_f32(masked_begin, S, scale);

        for (int64_t i = masked_begin; i < M; i++) {
            S[i] = -INFINITY;
        }

        // softmax
        // exclude known -INF S[..] values from max and loop
        // dont forget to set their SW values to zero
        {
            float max = -INFINITY;
            lm_ggml_vec_max_f32(masked_begin, &max, S);

            lm_ggml_float sum = 0.0;
            {
#ifdef LM_GGML_SOFT_MAX_ACCELERATE
                max = -max;
                vDSP_vsadd(S, 1, &max, S, 1, Mup);
                vvexpf(S, S, &Mup);
                lm_ggml_vec_sum_f32(Mup, &sum, S);
#else
                uint16_t   scvt[LM_GGML_SOFT_MAX_UNROLL]; UNUSED(scvt);
                lm_ggml_float sump[LM_GGML_SOFT_MAX_UNROLL] = { 0.0 };

                for (int i = 0; i < Mup; i += LM_GGML_SOFT_MAX_UNROLL) {
                    if (i >= masked_begin) {
                        break;
                    }
                    float * SS = S + i;

                    for (int j = 0; j < LM_GGML_SOFT_MAX_UNROLL; ++j) {
                        if (i + j >= masked_begin) {
                            break;
                        } else if (SS[j] == -INFINITY) {
                            SS[j] = 0.0f;
                        } else {
#ifndef LM_GGML_FLASH_ATTN_EXP_FP16
                            const float val = expf(SS[j] - max);
#else
                            lm_ggml_fp16_t s = LM_GGML_FP32_TO_FP16(SS[j] - max);
                            memcpy(&scvt[j], &s, sizeof(uint16_t));
                            const float val = LM_GGML_FP16_TO_FP32(table_exp_f16[scvt[j]]);
#endif
                            sump[j] += (lm_ggml_float)val;
                            SS[j] = val;
                        }
                    }
                }

                for (int i = 0; i < LM_GGML_SOFT_MAX_UNROLL; i++) {
                    sum += sump[i];
                }
#endif
            }

            assert(sum > 0.0);

            sum = 1.0/sum;
            lm_ggml_vec_scale_f32(masked_begin, S, sum);

#ifndef NDEBUG
            for (int i = 0; i < masked_begin; ++i) {
                assert(!isnan(S[i]));
                assert(!isinf(S[i]));
            }
#endif
        }

        for (int64_t ic = 0; ic < nev1; ++ic) {
            // dst indices
            const int i1 = iq1;
            const int i2 = iq2;
            const int i3 = iq3;

            // v indices
            const int iv2 = iq2 % nev2;
            const int iv3 = iq3;

            lm_ggml_vec_dot_f32(masked_begin,
                    (float *) ((char *) dst->data + (ic*nb0 + i1*nb1  + i2*nb2   + i3*nb3)),
                    (float *) ((char *) v->data   + (         ic*nbv1 + iv2*nbv2 + iv3*nbv3)),
                    S);
        }
    }
}

static void lm_ggml_compute_forward_flash_attn_f16(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * q,
        const struct lm_ggml_tensor * k,
        const struct lm_ggml_tensor * v,
        const bool masked,
        struct lm_ggml_tensor * dst) {
    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    LM_GGML_TENSOR_LOCALS(int64_t, neq, q,   ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nbq, q,   nb)
    LM_GGML_TENSOR_LOCALS(int64_t, nek, k,   ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nbk, k,   nb)
    LM_GGML_TENSOR_LOCALS(int64_t, nev, v,   ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nbv, v,   nb)
    LM_GGML_TENSOR_LOCALS(int64_t, ne,  dst, ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nb,  dst, nb)

    const int ith = params->ith;
    const int nth = params->nth;

    const int64_t D = neq0;
    const int64_t N = neq1;
    const int64_t P = nek1 - N;
    const int64_t M = P + N;

    const int Mup = lm_ggml_up(M, LM_GGML_SOFT_MAX_UNROLL);

    LM_GGML_ASSERT(ne0 == D);
    LM_GGML_ASSERT(ne1 == N);
    LM_GGML_ASSERT(P >= 0);

    LM_GGML_ASSERT(nbq0 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nbk0 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nbv0 == sizeof(lm_ggml_fp16_t));

    LM_GGML_ASSERT(neq0 == D);
    LM_GGML_ASSERT(nek0 == D);
    LM_GGML_ASSERT(nev1 == D);

    LM_GGML_ASSERT(neq1 == N);
    LM_GGML_ASSERT(nek1 == N + P);
    LM_GGML_ASSERT(nev1 == D);

    // dst cannot be transposed or permuted
    LM_GGML_ASSERT(nb0 == sizeof(float));
    LM_GGML_ASSERT(nb0 <= nb1);
    LM_GGML_ASSERT(nb1 <= nb2);
    LM_GGML_ASSERT(nb2 <= nb3);

    if (params->type == LM_GGML_TASK_INIT) {
        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // parallelize by q rows using lm_ggml_vec_dot_f32

    // total rows in q
    const int nr = neq1*neq2*neq3;

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    const float scale = 1.0f/sqrtf(D);

    //printf("P=%d N=%d D=%d ir0=%d ir1=%d scale = %f\n", P, N, D, ir0, ir1, scale);

    for (int ir = ir0; ir < ir1; ++ir) {
        // q indices
        const int iq3 = ir/(neq2*neq1);
        const int iq2 = (ir - iq3*neq2*neq1)/neq1;
        const int iq1 = (ir - iq3*neq2*neq1 - iq2*neq1);

        float * S = (float *) params->wdata + ith*(2*Mup + CACHE_LINE_SIZE_F32);

        for (int i = M; i < Mup; ++i) {
            S[i] = -INFINITY;
        }

        if (LM_GGML_VEC_DOT_UNROLL > 2 || nek1 % LM_GGML_VEC_DOT_UNROLL != 0) {
            for (int64_t ic = 0; ic < nek1; ++ic) {
                // k indices
                const int ik3 = iq3;
                const int ik2 = iq2 % nek2;
                const int ik1 = ic;

                // S indices
                const int i1 = ik1;

                lm_ggml_vec_dot_f16(neq0,
                        S + i1,
                        (lm_ggml_fp16_t *) ((char *) k->data + (ik1*nbk1 + ik2*nbk2 + ik3*nbk3)),
                        (lm_ggml_fp16_t *) ((char *) q->data + (iq1*nbq1 + iq2*nbq2 + iq3*nbq3)));
            }
        } else {
            for (int64_t ic = 0; ic < nek1; ic += LM_GGML_VEC_DOT_UNROLL) {
                // k indices
                const int ik3 = iq3;
                const int ik2 = iq2 % nek2;
                const int ik1 = ic;

                // S indices
                const int i1 = ik1;

                lm_ggml_vec_dot_f16_unroll(neq0, nbk1,
                        S + i1,
                        ((char *) k->data + (ik1*nbk1 + ik2*nbk2 + ik3*nbk3)),
                        (lm_ggml_fp16_t *) ((char *) q->data + (iq1*nbq1 + iq2*nbq2 + iq3*nbq3)));
            }
        }

        // scale
        lm_ggml_vec_scale_f32(nek1, S, scale);

        if (masked) {
            for (int64_t i = P; i < M; i++) {
                if (i > P + iq1) {
                    S[i] = -INFINITY;
                }
            }
        }

        // softmax
        // todo: exclude known -INF S[..] values from max and loop, assuming their results to be zero.
        // dont forget to set their S values to zero
        {
            float max = -INFINITY;
            lm_ggml_vec_max_f32(M, &max, S);

            lm_ggml_float sum = 0.0;
            {
#ifdef LM_GGML_SOFT_MAX_ACCELERATE
                max = -max;
                vDSP_vsadd(S, 1, &max, S, 1, Mup);
                vvexpf(S, S, &Mup);
                lm_ggml_vec_sum_f32(Mup, &sum, S);
#else
                uint16_t   scvt[LM_GGML_SOFT_MAX_UNROLL];
                lm_ggml_float sump[LM_GGML_SOFT_MAX_UNROLL] = { 0.0 };

                for (int i = 0; i < Mup; i += LM_GGML_SOFT_MAX_UNROLL) {
                    float * SS = S + i;

                    for (int j = 0; j < LM_GGML_SOFT_MAX_UNROLL; ++j) {
                        if (SS[j] == -INFINITY) {
                            SS[j] = 0.0f;
                        } else {
                            lm_ggml_fp16_t s = LM_GGML_FP32_TO_FP16(SS[j] - max);
                            memcpy(&scvt[j], &s, sizeof(uint16_t));
                            const float val = LM_GGML_FP16_TO_FP32(table_exp_f16[scvt[j]]);
                            sump[j] += (lm_ggml_float)val;
                            SS[j] = val;
                        }
                    }
                }

                for (int i = 0; i < LM_GGML_SOFT_MAX_UNROLL; i++) {
                    sum += sump[i];
                }
#endif
            }

            assert(sum > 0.0);

            sum = 1.0/sum;
            lm_ggml_vec_scale_f32(M, S, sum);

#ifndef NDEBUG
            for (int i = 0; i < M; ++i) {
                assert(!isnan(S[i]));
                assert(!isinf(S[i]));
            }
#endif
        }

        lm_ggml_fp16_t * S16 = (lm_ggml_fp16_t *) ((float *) params->wdata + ith*(2*Mup + CACHE_LINE_SIZE_F32) + Mup);

        for (int64_t i = 0; i < M; i++) {
            S16[i] = LM_GGML_FP32_TO_FP16(S[i]);
        }

        // todo: exclude known zero S[..] values from dot (reducing nev0 and increasing begin of v and S16).
        if (LM_GGML_VEC_DOT_UNROLL == 1 || (nev1 % LM_GGML_VEC_DOT_UNROLL != 0)) {
            for (int64_t ic = 0; ic < nev1; ++ic) {
                // dst indices
                const int i1 = iq1;
                const int i2 = iq2;
                const int i3 = iq3;

                // v indices
                const int iv2 = iq2 % nev2;
                const int iv3 = iq3;

                lm_ggml_vec_dot_f16(nev0,
                        (float *)       ((char *) dst->data + (ic*nb0 + i1*nb1  + i2*nb2   + i3*nb3)),
                        (lm_ggml_fp16_t *) ((char *) v->data   + (         ic*nbv1 + iv2*nbv2 + iv3*nbv3)),
                        S16);
            }
        } else {
            for (int64_t ic = 0; ic < nev1; ic += LM_GGML_VEC_DOT_UNROLL) {
                // dst indices
                const int i1 = iq1;
                const int i2 = iq2;
                const int i3 = iq3;

                // v indices
                const int iv2 = iq2 % nev2;
                const int iv3 = iq3;

                lm_ggml_vec_dot_f16_unroll(nev0, nbv1,
                        (float *) ((char *) dst->data + (ic*nb0 + i1*nb1  + i2*nb2   + i3*nb3)),
                        ((char *)             v->data + (         ic*nbv1 + iv2*nbv2 + iv3*nbv3)),
                        S16);
            }
        }
    }
}

static void lm_ggml_compute_forward_flash_attn(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * q,
        const struct lm_ggml_tensor * k,
        const struct lm_ggml_tensor * v,
        const bool masked,
        struct lm_ggml_tensor * dst) {
    switch (q->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_flash_attn_f16(params, q, k, v, masked, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_flash_attn_f32(params, q, k, v, masked, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_flash_ff

static void lm_ggml_compute_forward_flash_ff_f16(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * a,  // F16
        const struct lm_ggml_tensor * b0, // F16 fc_w
        const struct lm_ggml_tensor * b1, // F32 fc_b
        const struct lm_ggml_tensor * c0, // F16 proj_w
        const struct lm_ggml_tensor * c1, // F32 proj_b
        struct lm_ggml_tensor * dst) {
    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    LM_GGML_TENSOR_LOCALS(int64_t, nea,  a,   ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nba,  a,   nb)
    LM_GGML_TENSOR_LOCALS(int64_t, neb0, b0,  ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nbb0, b0,  nb)
    LM_GGML_TENSOR_LOCALS(int64_t, neb1, b1,  ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nbb1, b1,  nb)
    LM_GGML_TENSOR_LOCALS(int64_t, nec0, c0,  ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nbc0, c0,  nb)
    LM_GGML_TENSOR_LOCALS(int64_t, nec1, c1,  ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nbc1, c1,  nb)
    LM_GGML_TENSOR_LOCALS(int64_t, ne,   dst, ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nb,   dst, nb)

    const int ith = params->ith;
    const int nth = params->nth;

    const int64_t D = nea0;
    //const int64_t N = nea1;
    const int64_t M = neb01;

    LM_GGML_ASSERT(ne0 == nea0);
    LM_GGML_ASSERT(ne1 == nea1);
    LM_GGML_ASSERT(ne2 == nea2);

    LM_GGML_ASSERT(nba0  == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nbb00 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nbb10 == sizeof(float));
    LM_GGML_ASSERT(nbc00 == sizeof(lm_ggml_fp16_t));
    LM_GGML_ASSERT(nbc10 == sizeof(float));

    LM_GGML_ASSERT(neb00 == D);
    LM_GGML_ASSERT(neb01 == M);
    LM_GGML_ASSERT(neb10 == M);
    LM_GGML_ASSERT(neb11 == 1);

    LM_GGML_ASSERT(nec00 == M);
    LM_GGML_ASSERT(nec01 == D);
    LM_GGML_ASSERT(nec10 == D);
    LM_GGML_ASSERT(nec11 == 1);

    // dst cannot be transposed or permuted
    LM_GGML_ASSERT(nb0 == sizeof(float));
    LM_GGML_ASSERT(nb0 <= nb1);
    LM_GGML_ASSERT(nb1 <= nb2);
    LM_GGML_ASSERT(nb2 <= nb3);

    if (params->type == LM_GGML_TASK_INIT) {
        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // parallelize by a rows using lm_ggml_vec_dot_f32

    // total rows in a
    const int nr = nea1*nea2*nea3;

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    for (int ir = ir0; ir < ir1; ++ir) {
        // a indices
        const int ia3 = ir/(nea2*nea1);
        const int ia2 = (ir - ia3*nea2*nea1)/nea1;
        const int ia1 = (ir - ia3*nea2*nea1 - ia2*nea1);

        float * S = (float *) params->wdata + ith*(2*M + CACHE_LINE_SIZE_F32);

        for (int64_t ic = 0; ic < neb01; ++ic) {
            // b0 indices
            const int ib03 = ia3;
            const int ib02 = ia2;
            const int ib01 = ic;

            // S indices
            const int i1 = ib01;

            lm_ggml_vec_dot_f16(nea0,
                    S + i1,
                    (lm_ggml_fp16_t *) ((char *) b0->data + (ib01*nbb01 + ib02*nbb02 + ib03*nbb03)),
                    (lm_ggml_fp16_t *) ((char *)  a->data + ( ia1*nba1  +  ia2*nba2  +  ia3*nba3)));
        }

        lm_ggml_vec_add_f32(neb01, S, S, (float *) b1->data);
        //lm_ggml_vec_gelu_f32(neb01, S, S);

        lm_ggml_fp16_t * S16 = (lm_ggml_fp16_t *) ((float *) params->wdata + ith*(2*M + CACHE_LINE_SIZE_F32) + M);

        for (int64_t i = 0; i < M; i++) {
            S16[i] = LM_GGML_FP32_TO_FP16(S[i]);
        }

        lm_ggml_vec_gelu_f16(neb01, S16, S16);

        {
            // dst indices
            const int i1 = ia1;
            const int i2 = ia2;
            const int i3 = ia3;

            for (int64_t ic = 0; ic < nec01; ++ic) {

                lm_ggml_vec_dot_f16(neb01,
                        (float *)       ((char *) dst->data + (ic*nb0 + i1*nb1   + i2*nb2   + i3*nb3)),
                        (lm_ggml_fp16_t *) ((char *) c0->data  + (         ic*nbc01 + i2*nbc02 + i3*nbc03)),
                        S16);
            }

            lm_ggml_vec_add_f32(nec01,
                    (float *) ((char *) dst->data + (i1*nb1 + i2*nb2 + i3*nb3)),
                    (float *) ((char *) dst->data + (i1*nb1 + i2*nb2 + i3*nb3)),
                    (float *) c1->data);
        }
    }
}

static void lm_ggml_compute_forward_flash_ff(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * a,
        const struct lm_ggml_tensor * b0,
        const struct lm_ggml_tensor * b1,
        const struct lm_ggml_tensor * c0,
        const struct lm_ggml_tensor * c1,
        struct lm_ggml_tensor * dst) {
    switch (b0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_flash_ff_f16(params, a, b0, b1, c0, c1, dst);
            } break;
        case LM_GGML_TYPE_F32:
            {
                LM_GGML_ASSERT(false); // TODO
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_flash_attn_back

static void lm_ggml_compute_forward_flash_attn_back_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * q,
        const struct lm_ggml_tensor * k,
        const struct lm_ggml_tensor * v,
        const struct lm_ggml_tensor * d,
        const bool masked,
              struct lm_ggml_tensor * dst) {
    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    LM_GGML_TENSOR_LOCALS(int64_t, neq, q,   ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nbq, q,   nb)
    LM_GGML_TENSOR_LOCALS(int64_t, nek, k,   ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nbk, k,   nb)
    LM_GGML_TENSOR_LOCALS(int64_t, nev, v,   ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nbv, v,   nb)
    LM_GGML_TENSOR_LOCALS(int64_t, ned, d,   ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nbd, d,   nb)
    LM_GGML_TENSOR_LOCALS(int64_t, ne,  dst, ne)
    LM_GGML_TENSOR_LOCALS(size_t,  nb,  dst, nb)

    const int ith = params->ith;
    const int nth = params->nth;

    const int64_t D = neq0;
    const int64_t N = neq1;
    const int64_t P = nek1 - N;
    const int64_t M = P + N;

    const int Mup  = lm_ggml_up(M, LM_GGML_SOFT_MAX_UNROLL);
    const int mxDM = MAX(D, Mup);

    // LM_GGML_ASSERT(ne0 == D);
    // LM_GGML_ASSERT(ne1 == N);
    LM_GGML_ASSERT(P >= 0);

    LM_GGML_ASSERT(nbq0 == sizeof(float));
    LM_GGML_ASSERT(nbk0 == sizeof(float));
    LM_GGML_ASSERT(nbv0 == sizeof(float));

    LM_GGML_ASSERT(neq0 == D);
    LM_GGML_ASSERT(nek0 == D);
    LM_GGML_ASSERT(nev1 == D);
    LM_GGML_ASSERT(ned0 == D);

    LM_GGML_ASSERT(neq1 == N);
    LM_GGML_ASSERT(nek1 == N + P);
    LM_GGML_ASSERT(nev1 == D);
    LM_GGML_ASSERT(ned1 == N);

    // dst cannot be transposed or permuted
    LM_GGML_ASSERT(nb0 == sizeof(float));
    LM_GGML_ASSERT(nb0 <= nb1);
    LM_GGML_ASSERT(nb1 <= nb2);
    LM_GGML_ASSERT(nb2 <= nb3);

    if (params->type == LM_GGML_TASK_INIT) {
        if (ith == 0) {
            memset(dst->data, 0, nb0*ne0*ne1*ne2*ne3);
        }
        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int64_t elem_q = lm_ggml_nelements(q);
    const int64_t elem_k = lm_ggml_nelements(k);

    enum lm_ggml_type result_type = dst->type;
    LM_GGML_ASSERT(lm_ggml_blck_size(result_type) == 1);
    const size_t tsize = lm_ggml_type_size(result_type);

    const size_t offs_q = 0;
    const size_t offs_k = offs_q + LM_GGML_PAD(elem_q * tsize, LM_GGML_MEM_ALIGN);
    const size_t offs_v = offs_k + LM_GGML_PAD(elem_k * tsize, LM_GGML_MEM_ALIGN);

    void * grad_q = (char *) dst->data;
    void * grad_k = (char *) dst->data + offs_k;
    void * grad_v = (char *) dst->data + offs_v;

    const size_t nbgq1 = nb0*neq0;
    const size_t nbgq2 = nb0*neq0*neq1;
    const size_t nbgq3 = nb0*neq0*neq1*neq2;

    const size_t nbgk1 = nb0*nek0;
    const size_t nbgk2 = nb0*nek0*nek1;
    const size_t nbgk3 = nb0*nek0*nek1*neq2;

    const size_t nbgv1 = nb0*nev0;
    const size_t nbgv2 = nb0*nev0*nev1;
    const size_t nbgv3 = nb0*nev0*nev1*neq2;

    // parallelize by k rows using lm_ggml_vec_dot_f32

    // total rows in k
    const int nr = nek2*nek3;

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    const float scale = 1.0f/sqrtf(D);

    //printf("P=%d N=%d D=%d ir0=%d ir1=%d scale = %f\n", P, N, D, ir0, ir1, scale);

    // how often k2 (and v2) is repeated in q2
    int nrep = neq2/nek2;

    for (int ir = ir0; ir < ir1; ++ir) {
        // q indices
        const int ik3 = ir/(nek2);
        const int ik2 = ir - ik3*nek2;

        const int iq3 = ik3;
        const int id3 = ik3;
        const int iv3 = ik3;
        const int iv2 = ik2;

        for (int irep = 0; irep < nrep; ++irep) {
            const int iq2 = ik2 + irep*nek2;
            const int id2 = iq2;

            // (ik2 + irep*nek2) % nek2 == ik2
            for (int iq1 = 0; iq1 < neq1; ++iq1) {
                const int id1 = iq1;

                // not sure about CACHE_LINE_SIZE_F32..
                // - maybe it must not be multiplied by 2 and excluded from .. in SM 1*(..) offset?
                float * S  = (float *) params->wdata + ith*2*(mxDM + CACHE_LINE_SIZE_F32) + 0*(mxDM+CACHE_LINE_SIZE_F32);
                float * SM = (float *) params->wdata + ith*2*(mxDM + CACHE_LINE_SIZE_F32) + 1*(mxDM+CACHE_LINE_SIZE_F32);

                for (int i = M; i < Mup; ++i) {
                    S[i] = -INFINITY;
                }

                const int64_t masked_begin = masked ? (P + iq1 + 1) : M;
                for (int64_t ic = 0; ic < masked_begin; ++ic) {
                    // k indices
                    const int ik1 = ic;

                    // S indices
                    const int i1 = ik1;

                    lm_ggml_vec_dot_f32(neq0,
                            S + i1,
                            (float *) ((char *) k->data + (ik1*nbk1 + ik2*nbk2 + ik3*nbk3)),
                            (float *) ((char *) q->data + (iq1*nbq1 + iq2*nbq2 + iq3*nbq3)));
                }

                // scale
                lm_ggml_vec_scale_f32(masked_begin, S, scale);

                for (int64_t i = masked_begin; i < M; i++) {
                    S[i] = -INFINITY;
                }

                // softmax
                // exclude known -INF S[..] values from max and loop
                // dont forget to set their SM values to zero
                {
                    float max = -INFINITY;
                    lm_ggml_vec_max_f32(masked_begin, &max, S);

                    lm_ggml_float sum = 0.0;
                    {
#ifdef LM_GGML_SOFT_MAX_ACCELERATE
                        max = -max;
                        vDSP_vsadd(SM, 1, &max, SM, 1, Mup);
                        vvexpf(SM, SM, &Mup);
                        lm_ggml_vec_sum_f32(Mup, &sum, SM);
#else
                        uint16_t   scvt[LM_GGML_SOFT_MAX_UNROLL]; UNUSED(scvt);
                        lm_ggml_float sump[LM_GGML_SOFT_MAX_UNROLL] = { 0.0 };

                        for (int i = 0; i < Mup; i += LM_GGML_SOFT_MAX_UNROLL) {
                            if (i >= masked_begin) {
                                break;
                            }
                            float * SR =  S + i;
                            float * SW = SM + i;

                            for (int j = 0; j < LM_GGML_SOFT_MAX_UNROLL; ++j) {
                                if (i + j >= masked_begin) {
                                    break;
                                } else if (SR[j] == -INFINITY) {
                                    SW[j] = 0.0f;
                                } else {
#ifndef LM_GGML_FLASH_ATTN_EXP_FP16
                                    const float val = expf(SR[j] - max);
#else
                                    lm_ggml_fp16_t s = LM_GGML_FP32_TO_FP16(SR[j] - max);
                                    memcpy(&scvt[j], &s, sizeof(uint16_t));
                                    const float val = LM_GGML_FP16_TO_FP32(table_exp_f16[scvt[j]]);
#endif
                                    sump[j] += (lm_ggml_float)val;
                                    SW[j] = val;
                                }
                            }
                        }

                        for (int i = 0; i < LM_GGML_SOFT_MAX_UNROLL; i++) {
                            sum += sump[i];
                        }
#endif
                    }

                    assert(sum > 0.0);

                    sum = 1.0/sum;
                    lm_ggml_vec_scale_f32(masked_begin, SM, sum);

                }

                // step-by-step explanation
                {
                    // forward-process                    shape      grads from backward process
                    // parallel_for ik2,ik3:
                    //  for irep:
                    //   iq2 = ik2 + irep*nek2
                    //   k[:D,:M,:,:]                     [D,M,:,:]  grad[k][:D,:M,ik2,ik3]  += grad[kcur]
                    //   q[:D,:N,:,:]                     [D,N,:,:]  grad[q][:D,iq1,iq2,iq3] += grad[qcur]
                    //   v[:M,:D,:,:]                     [M,D,:,:]  grad[v][:M,:D,iv2,iv3]  += grad[vcur]
                    //   for iq1:
                    //    kcur   = k[:D,:M,ik2,ik3]       [D,M,1,1]  grad[kcur] = grad[S1].T @ qcur
                    //    qcur   = q[:D,iq1,iq2,iq3]      [D,1,1,1]  grad[qcur] = grad[S1]   @ kcur
                    //    vcur   = v[:M,:D,iv2,iv3]       [M,D,1,1]  grad[vcur] = grad[S5].T @ S4
                    //    S0     = -Inf                   [D,1,1,1]
                    //   ~S1[i]  = dot(kcur[:D,i], qcur)
                    //    S1     = qcur @ kcur.T          [M,1,1,1]  grad[S1]   = grad[S2] * scale
                    //    S2     = S1 * scale             [M,1,1,1]  grad[S2]   = diag_mask_zero(grad[S3], P)
                    //    S3     = diag_mask_inf(S2, P)   [M,1,1,1]  grad[S3]   = S4 * (grad[S4] - dot(S4, grad[S4]))
                    //    S4     = softmax(S3)            [M,1,1,1]  grad[S4]   = grad[S5] @ vcur
                    //   ~S5[i]  = dot(vcur[:,i], S4)
                    //    S5     = S4 @ vcur.T            [D,1,1,1]  grad[S5]   = d[:D,id1,id2,id3]
                    //   ~dst[i,iq1,iq2,iq3]  = S5[i]              ^
                    //    dst[:D,iq1,iq2,iq3] = S5                 | grad[dst[:D,iq1,iq2,iq3]] = d[:D,id1,id2,id3]
                    // dst                               backward-/ grad[dst]                 = d
                    //
                    // output gradients with their dependencies:
                    //
                    // grad[kcur] = grad[S1].T @ qcur
                    // grad[S1]   = diag_mask_zero(grad[S3], P) * scale
                    // grad[S3]   = S4 * (grad[S4] - dot(S4, grad[S4]))
                    // grad[S4]   = grad[S5] @ vcur
                    // grad[S4]   = d[:D,id1,id2,id3] @ vcur
                    // grad[qcur] = grad[S1]   @ kcur
                    // grad[vcur] = grad[S5].T @ S4
                    // grad[vcur] = d[:D,id1,id2,id3].T @ S4
                    //
                    // in post-order:
                    //
                    // S1         = qcur @ kcur.T
                    // S2         = S1 * scale
                    // S3         = diag_mask_inf(S2, P)
                    // S4         = softmax(S3)
                    // grad[S4]   = d[:D,id1,id2,id3] @ vcur
                    // grad[S3]   = S4 * (grad[S4] - dot(S4, grad[S4]))
                    // grad[S1]   = diag_mask_zero(grad[S3], P) * scale
                    // grad[qcur] = grad[S1]   @ kcur
                    // grad[kcur] = grad[S1].T @ qcur
                    // grad[vcur] = d[:D,id1,id2,id3].T @ S4
                    //
                    // using less variables (SM=S4):
                    //
                    // S             = diag_mask_inf(qcur @ kcur.T * scale, P)
                    // SM            = softmax(S)
                    // S             = d[:D,iq1,iq2,iq3] @ vcur
                    // dot_SM_gradSM = dot(SM, S)
                    // S             = SM * (S - dot(SM, S))
                    // S             = diag_mask_zero(S, P) * scale
                    //
                    // grad[q][:D,iq1,iq2,iq3] += S   @ kcur
                    // grad[k][:D,:M,ik2,ik3]  += S.T @ qcur
                    // grad[v][:M,:D,iv2,iv3]  += d[:D,id1,id2,id3].T @ SM
                }

                // S = gradSM = d[:D,id1,id2,id3] @ vcur[:,:,iv2,iv3]
                // S = d[:D,id1,id2,id3] @ vcur[:,:,iv2,iv3]
                // for ic:
                //   S[:M] += vcur[:M,ic,iv2,iv3] * d[ic,id1,id2,id3]
                // exclude known future zero S[..] values from operation
                lm_ggml_vec_set_f32(masked_begin, S, 0);
                for (int64_t ic = 0; ic < D; ++ic) {
                    lm_ggml_vec_mad_f32(masked_begin,
                            S,
                             (float *) ((char *) v->data + (          ic*nbv1  + iv2*nbv2 + iv3*nbv3)),
                            *(float *) ((char *) d->data + (ic*nbd0 + id1*nbd1 + id2*nbd2 + id3*nbd3)));
                }

                // S = SM * (S - dot(SM, S))
                float dot_SM_gradSM = 0;
                lm_ggml_vec_dot_f32 (masked_begin, &dot_SM_gradSM, SM, S);
                lm_ggml_vec_acc1_f32(M, S, -dot_SM_gradSM);
                lm_ggml_vec_mul_f32 (masked_begin, S, S, SM);

                // S = diag_mask_zero(S, P) * scale
                // already done by above lm_ggml_vec_set_f32

                // exclude known zero S[..] values from operation
                lm_ggml_vec_scale_f32(masked_begin, S, scale);

                // S    shape [M,1]
                // SM   shape [M,1]
                // kcur shape [D,M]
                // qcur shape [D,1]
                // vcur shape [M,D]

                // grad[q][:D,iq1,iq2,iq3] += S @ kcur
                // grad[q][:D,iq1,iq2,iq3] += shape[M,1] @ shape[D,M]
                // for ic:
                //  grad[q][:D,iq1,iq2,iq3] += S[ic] * kcur[:D,ic,ik2,ik3]
                // exclude known zero S[..] values from loop
                for (int64_t ic = 0; ic < masked_begin; ++ic) {
                    lm_ggml_vec_mad_f32(D,
                            (float *) ((char *) grad_q  + (iq1*nbgq1 + iq2*nbgq2  + iq3*nbgq3)),
                            (float *) ((char *) k->data + (ic*nbk1   + ik2*nbk2   + ik3*nbk3)),
                            S[ic]);
                }

                // grad[k][:D,:M,iq2,iq3] += S.T @ qcur
                // for ic:
                //  grad[k][:D,ic,iq2,iq3] += S.T[0,ic] * qcur[:D,0]
                //  grad[k][:D,ic,iq2,iq3] += S[ic]     * qcur[:D,0]
                // exclude known zero S[..] values from loop
                for (int64_t ic = 0; ic < masked_begin; ++ic) {
                    lm_ggml_vec_mad_f32(D,
                            (float *) ((char *) grad_k  + (ic*nbgk1  + ik2*nbgk2  + ik3*nbgk3)),
                            (float *) ((char *) q->data + (iq1*nbq1  + iq2*nbq2   + iq3*nbq3)),
                            S[ic]);
                }

                // grad[v][:M,:D,iv2,iv3] += d[:D,id1,id2,id3].T       @ SM
                // for ic:
                //  grad[v][:M,ic,iv2,iv3] += d[:D,id1,id2,id3].T[0,ic] * SM[:M]
                //  grad[v][:M,ic,iv2,iv3] += d[ic,id1,id2,id3]         * SM[:M]
                // exclude known zero SM[..] values from mad
                for (int64_t ic = 0; ic < D; ++ic) {
                    lm_ggml_vec_mad_f32(masked_begin,
                            (float *) ((char *) grad_v   + (          ic*nbgv1 + iv2*nbgv2 + iv3*nbgv3)),
                            SM,
                            *(float *) ((char *) d->data + (ic*nbd0 + id1*nbd1 + id2*nbd2  + id3*nbd3)));
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_flash_attn_back(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * q,
        const struct lm_ggml_tensor * k,
        const struct lm_ggml_tensor * v,
        const struct lm_ggml_tensor * d,
        const bool masked,
        struct lm_ggml_tensor * dst) {
    switch (q->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_flash_attn_back_f32(params, q, k, v, d, masked, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_win_part

static void lm_ggml_compute_forward_win_part_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_TENSOR_LOCALS(int64_t, ne0, src0, ne)
    LM_GGML_TENSOR_LOCALS(int64_t, ne,  dst,  ne)

    const int32_t nep0 = ((const int32_t *)(dst->op_params))[0];
    const int32_t nep1 = ((const int32_t *)(dst->op_params))[1];
    const int32_t w    = ((const int32_t *)(dst->op_params))[2];

    assert(ne00 == ne0);
    assert(ne3  == nep0*nep1);

    // TODO: optimize / multi-thread
    for (int py = 0; py < nep1; ++py) {
        for (int px = 0; px < nep0; ++px) {
            const int64_t i3 = py*nep0 + px;
            for (int64_t i2 = 0; i2 < ne2; ++i2) {
                for (int64_t i1 = 0; i1 < ne1; ++i1) {
                    for (int64_t i0 = 0; i0 < ne0; ++i0) {
                        const int64_t i02 = py*w + i2;
                        const int64_t i01 = px*w + i1;
                        const int64_t i00 = i0;

                        const int64_t i = i3*ne2*ne1*ne0 + i2*ne1*ne0    + i1*ne0   + i0;
                        const int64_t j =                  i02*ne01*ne00 + i01*ne00 + i00;

                        if (py*w + i2 >= ne02 || px*w + i1 >= ne01) {
                            ((float *) dst->data)[i] = 0.0f;
                        } else {
                            ((float *) dst->data)[i] = ((float *) src0->data)[j];
                        }
                    }
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_win_part(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_win_part_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_win_unpart

static void lm_ggml_compute_forward_win_unpart_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    LM_GGML_TENSOR_LOCALS(int64_t, ne0, src0, ne)
    LM_GGML_TENSOR_LOCALS(int64_t, ne,  dst,  ne)

    const int32_t w = ((const int32_t *)(dst->op_params))[0];

    // padding
    const int px = (w - ne1%w)%w;
    //const int py = (w - ne2%w)%w;

    const int npx = (px + ne1)/w;
    //const int npy = (py + ne2)/w;

    assert(ne0 == ne00);

    // TODO: optimize / multi-thread
    for (int64_t i2 = 0; i2 < ne2; ++i2) {
        for (int64_t i1 = 0; i1 < ne1; ++i1) {
            for (int64_t i0 = 0; i0 < ne0; ++i0) {
                const int ip2 = i2/w;
                const int ip1 = i1/w;

                const int64_t i02 = i2%w;
                const int64_t i01 = i1%w;
                const int64_t i00 = i0;

                const int64_t i = (ip2*npx + ip1)*ne02*ne01*ne00 + i02*ne01*ne00 + i01*ne00 + i00;
                const int64_t j =                                  i2*ne1*ne0    + i1*ne0   + i0;

                ((float *) dst->data)[j] = ((float *) src0->data)[i];
            }
        }
    }
}

static void lm_ggml_compute_forward_win_unpart(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_win_unpart_f32(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

//gmml_compute_forward_unary

static void lm_ggml_compute_forward_unary(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    const enum lm_ggml_unary_op op = lm_ggml_get_unary_op(dst);

    switch (op) {
        case LM_GGML_UNARY_OP_ABS:
            {
                lm_ggml_compute_forward_abs(params, src0, dst);
            } break;
        case LM_GGML_UNARY_OP_SGN:
            {
                lm_ggml_compute_forward_sgn(params, src0, dst);
            } break;
        case LM_GGML_UNARY_OP_NEG:
            {
                lm_ggml_compute_forward_neg(params, src0, dst);
            } break;
        case LM_GGML_UNARY_OP_STEP:
            {
                lm_ggml_compute_forward_step(params, src0, dst);
            } break;
        case LM_GGML_UNARY_OP_TANH:
            {
                lm_ggml_compute_forward_tanh(params, src0, dst);
            } break;
        case LM_GGML_UNARY_OP_ELU:
            {
                lm_ggml_compute_forward_elu(params, src0, dst);
            } break;
        case LM_GGML_UNARY_OP_RELU:
            {
                lm_ggml_compute_forward_relu(params, src0, dst);
            } break;
        case LM_GGML_UNARY_OP_GELU:
            {
                lm_ggml_compute_forward_gelu(params, src0, dst);
            } break;
        case LM_GGML_UNARY_OP_GELU_QUICK:
            {
                lm_ggml_compute_forward_gelu_quick(params, src0, dst);
            } break;
        case LM_GGML_UNARY_OP_SILU:
            {
                lm_ggml_compute_forward_silu(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_get_rel_pos

static void lm_ggml_compute_forward_get_rel_pos_f16(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    // ref: https://github.com/facebookresearch/segment-anything/blob/main/segment_anything/modeling/image_encoder.py#L292-L322

    LM_GGML_TENSOR_UNARY_OP_LOCALS

    const int64_t w = ne1;

    lm_ggml_fp16_t * src0_data = (lm_ggml_fp16_t *) src0->data;
    lm_ggml_fp16_t * dst_data  = (lm_ggml_fp16_t *) dst->data;

    for (int64_t i2 = 0; i2 < ne2; ++i2) {
        for (int64_t i1 = 0; i1 < ne1; ++i1) {
            const int64_t pos = (w - i1 - 1) + i2;
            for (int64_t i0 = 0; i0 < ne0; ++i0) {
                dst_data[i2*ne1*ne0 + i1*ne0 + i0] = src0_data[pos*ne00 + i0];
            }
        }
    }
}

static void lm_ggml_compute_forward_get_rel_pos(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F16:
            {
                lm_ggml_compute_forward_get_rel_pos_f16(params, src0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_add_rel_pos

static void lm_ggml_compute_forward_add_rel_pos_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        const struct lm_ggml_tensor * src2,
        struct lm_ggml_tensor * dst) {

    const bool inplace = (bool) ((int32_t *) dst->op_params)[0];
    if (!inplace && params->type == LM_GGML_TASK_INIT) {
        memcpy((char *) dst->data, (char *) src0->data, lm_ggml_nbytes(dst));
        return;
    }
    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    int64_t t0 = lm_ggml_perf_time_us();
    UNUSED(t0);

    // ref: https://github.com/facebookresearch/segment-anything/blob/main/segment_anything/modeling/image_encoder.py#L357-L359

    float * src1_data = (float *) src1->data;
    float * src2_data = (float *) src2->data;
    float * dst_data  = (float *) dst->data;

    const int64_t ne10 = src1->ne[0];
    const int64_t ne11 = src1->ne[1];
    const int64_t ne12 = src1->ne[2];
    const int64_t ne13 = src1->ne[3];

    const int ith = params->ith;
    const int nth = params->nth;

    // total patches in dst
    const int np = ne13;

    // patches per thread
    const int dp = (np + nth - 1)/nth;

    // patch range for this thread
    const int ip0 = dp*ith;
    const int ip1 = MIN(ip0 + dp, np);

    for (int64_t i13 = ip0; i13 < ip1; ++i13) {
        for (int64_t i12 = 0; i12 < ne12; ++i12) {
            for (int64_t i11 = 0; i11 < ne11; ++i11) {
                const int64_t jp1 = i13*ne12*ne11*ne10 + i12*ne11*ne10 + i11*ne10;
                for (int64_t i10 = 0; i10 < ne10; ++i10) {
                    const int64_t jp0  = jp1 + i10;
                    const float src1_e = src1_data[jp0];
                    const float src2_e = src2_data[jp0];

                    const int64_t jdh = jp0 * ne10;
                    const int64_t jdw = jdh - (ne10 - 1) * i10;

                    for (int64_t j = 0; j < ne10; ++j) {
                        dst_data[jdh + j     ] += src2_e;
                        dst_data[jdw + j*ne10] += src1_e;
                    }
                }
            }
        }
    }
}

static void lm_ggml_compute_forward_add_rel_pos(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        const struct lm_ggml_tensor * src2,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_add_rel_pos_f32(params, src0, src1, src2, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_map_unary

static void lm_ggml_compute_forward_map_unary_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst,
        const lm_ggml_unary_op_f32_t fun) {
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int n  = lm_ggml_nrows(src0);
    const int nc = src0->ne[0];

    assert( dst->nb[0] == sizeof(float));
    assert(src0->nb[0] == sizeof(float));

    for (int i = 0; i < n; i++) {
        fun(nc,
                (float *) ((char *) dst->data  + i*( dst->nb[1])),
                (float *) ((char *) src0->data + i*(src0->nb[1])));
    }
}

static void lm_ggml_compute_forward_map_unary(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        struct lm_ggml_tensor * dst,
        const lm_ggml_unary_op_f32_t fun) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_map_unary_f32(params, src0, dst, fun);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_map_binary

static void lm_ggml_compute_forward_map_binary_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst,
        const lm_ggml_binary_op_f32_t fun) {
    assert(params->ith == 0);
    assert(lm_ggml_are_same_shape(src0, src1) && lm_ggml_are_same_shape(src0, dst));

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const int n  = lm_ggml_nrows(src0);
    const int nc = src0->ne[0];

    assert( dst->nb[0] == sizeof(float));
    assert(src0->nb[0] == sizeof(float));
    assert(src1->nb[0] == sizeof(float));

    for (int i = 0; i < n; i++) {
        fun(nc,
                (float *) ((char *) dst->data  + i*( dst->nb[1])),
                (float *) ((char *) src0->data + i*(src0->nb[1])),
                (float *) ((char *) src1->data + i*(src1->nb[1])));
    }
}

static void lm_ggml_compute_forward_map_binary(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst,
        const lm_ggml_binary_op_f32_t fun) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_map_binary_f32(params, src0, src1, dst, fun);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_map_custom1

static void lm_ggml_compute_forward_map_custom1_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * a,
        struct lm_ggml_tensor * dst,
        const lm_ggml_custom1_op_f32_t fun) {
    assert(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    fun(dst, a);
}

// lm_ggml_compute_forward_map_custom2

static void lm_ggml_compute_forward_map_custom2_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * a,
        const struct lm_ggml_tensor * b,
        struct lm_ggml_tensor * dst,
        const lm_ggml_custom2_op_f32_t fun) {
    assert(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    fun(dst, a, b);
}

// lm_ggml_compute_forward_map_custom3

static void lm_ggml_compute_forward_map_custom3_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * a,
        const struct lm_ggml_tensor * b,
        const struct lm_ggml_tensor * c,
        struct lm_ggml_tensor * dst,
        const lm_ggml_custom3_op_f32_t fun) {
    assert(params->ith == 0);

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    fun(dst, a, b, c);
}

// lm_ggml_compute_forward_map_custom1

static void lm_ggml_compute_forward_map_custom1(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * a,
              struct lm_ggml_tensor * dst) {
    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    struct lm_ggml_map_custom1_op_params * p = (struct lm_ggml_map_custom1_op_params *) dst->op_params;

    p->fun(dst, a, params->ith, params->nth, p->userdata);
}

// lm_ggml_compute_forward_map_custom2

static void lm_ggml_compute_forward_map_custom2(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * a,
        const struct lm_ggml_tensor * b,
              struct lm_ggml_tensor * dst) {
    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    struct lm_ggml_map_custom2_op_params * p = (struct lm_ggml_map_custom2_op_params *) dst->op_params;

    p->fun(dst, a, b, params->ith, params->nth, p->userdata);
}

// lm_ggml_compute_forward_map_custom3

static void lm_ggml_compute_forward_map_custom3(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * a,
        const struct lm_ggml_tensor * b,
        const struct lm_ggml_tensor * c,
              struct lm_ggml_tensor * dst) {
    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    struct lm_ggml_map_custom3_op_params * p = (struct lm_ggml_map_custom3_op_params *) dst->op_params;

    p->fun(dst, a, b, c, params->ith, params->nth, p->userdata);
}

// lm_ggml_compute_forward_cross_entropy_loss

static void lm_ggml_compute_forward_cross_entropy_loss_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_is_contiguous(src0));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(src1));
    LM_GGML_ASSERT(lm_ggml_is_scalar(dst));
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, src1));

    const int ith = params->ith;
    const int nth = params->nth;

    float * sums = (float *) params->wdata;

    // TODO: handle transposed/permuted matrices
    const int nc = src0->ne[0];
    const int nr = lm_ggml_nrows(src0);

    LM_GGML_ASSERT(params->wsize >= sizeof(float) * (nth + nth * nc));

    if (params->type == LM_GGML_TASK_INIT) {
        if (ith == 0) {
            memset(sums, 0, sizeof(float) * (nth + nth * nc));
        }
        return;
    }

    if (params->type == LM_GGML_TASK_FINALIZE) {
        if (ith == 0) {
            float * dp = (float *) dst->data;
            lm_ggml_vec_sum_f32(nth, dp, sums);
            dp[0] *= -1.0f / (float) nr;
        }
        return;
    }

    const double eps = 1e-9;

    // rows per thread
    const int dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int ir0 = dr*ith;
    const int ir1 = MIN(ir0 + dr, nr);

    for (int i1 = ir0; i1 < ir1; i1++) {
        float * s0 = (float *)((char *) src0->data + i1*src0->nb[1]);
        float * s1 = (float *)((char *) src1->data + i1*src1->nb[1]);
        float * st = ((float *) params->wdata) + nth + ith*nc;

#ifndef NDEBUG
        for (int i = 0; i < nc; ++i) {
            //printf("p[%d] = %f\n", i, p[i]);
            assert(!isnan(s0[i]));
            assert(!isnan(s1[i]));
        }
#endif
        // soft_max
        lm_ggml_float sum = 0.0;
        {
            float max = -INFINITY;
            lm_ggml_vec_max_f32(nc, &max, s0);

            uint16_t scvt; UNUSED(scvt);
            for (int i = 0; i < nc; i++) {
                if (s0[i] == -INFINITY) {
                    st[i] = 0.0f;
                } else {
#ifndef LM_GGML_CROSS_ENTROPY_EXP_FP16
                    const float s = s0[i] - max;
                    const float val = expf(s);
#else
                    lm_ggml_fp16_t s = LM_GGML_FP32_TO_FP16(s0[i] - max);
                    memcpy(&scvt, &s, sizeof(scvt));
                    const float val = LM_GGML_FP16_TO_FP32(table_exp_f16[scvt]);
#endif
                    sum += (lm_ggml_float)val;
                    st[i] = val;
                }
            }

            assert(sum > 0.0);
            // sum = 1.0/sum;
        }
        // avoid log(0) by rescaling from [0..1] to [eps..1]
        sum = (1.0 - eps) / sum;
        lm_ggml_vec_scale_f32(nc, st, sum);
        lm_ggml_vec_add1_f32(nc, st, st, eps);
        lm_ggml_vec_log_f32(nc, st, st);
        lm_ggml_vec_mul_f32(nc, st, st, s1);

        float st_sum = 0;
        lm_ggml_vec_sum_f32(nc, &st_sum, st);
        sums[ith] += st_sum;

#ifndef NDEBUG
        for (int i = 0; i < nc; ++i) {
            assert(!isnan(st[i]));
            assert(!isinf(st[i]));
        }
#endif
    }

}

static void lm_ggml_compute_forward_cross_entropy_loss(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_cross_entropy_loss_f32(params, src0, src1, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

// lm_ggml_compute_forward_cross_entropy_loss_back

static void lm_ggml_compute_forward_cross_entropy_loss_back_f32(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        const struct lm_ggml_tensor * opt0,
        struct lm_ggml_tensor * dst) {
    LM_GGML_ASSERT(lm_ggml_is_contiguous(dst));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(src0));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(src1));
    LM_GGML_ASSERT(lm_ggml_is_contiguous(opt0));
    LM_GGML_ASSERT(lm_ggml_are_same_shape(src0, src1) && lm_ggml_are_same_shape(src0, dst));

    const int64_t ith = params->ith;
    const int64_t nth = params->nth;

    if (params->type == LM_GGML_TASK_INIT || params->type == LM_GGML_TASK_FINALIZE) {
        return;
    }

    const double eps = 1e-9;

    // TODO: handle transposed/permuted matrices
    const int64_t nc = src0->ne[0];
    const int64_t nr = lm_ggml_nrows(src0);

    // rows per thread
    const int64_t dr = (nr + nth - 1)/nth;

    // row range for this thread
    const int64_t ir0 = dr*ith;
    const int64_t ir1 = MIN(ir0 + dr, nr);

    float * d   = (float *) opt0->data;

    for (int64_t i1 = ir0; i1 < ir1; i1++) {
        float * ds0 = (float *)((char *) dst->data  + i1*dst->nb[1]);
        float * s0  = (float *)((char *) src0->data + i1*src0->nb[1]);
        float * s1  = (float *)((char *) src1->data + i1*src1->nb[1]);

#ifndef NDEBUG
        for (int i = 0; i < nc; ++i) {
            //printf("p[%d] = %f\n", i, p[i]);
            assert(!isnan(s0[i]));
            assert(!isnan(s1[i]));
        }
#endif

        // soft_max
        lm_ggml_float sum = 0.0;
        {
            float max = -INFINITY;
            lm_ggml_vec_max_f32(nc, &max, s0);

            uint16_t scvt; UNUSED(scvt);
            for (int i = 0; i < nc; i++) {
                if (s0[i] == -INFINITY) {
                    ds0[i] = 0.0f;
                } else {
#ifndef LM_GGML_CROSS_ENTROPY_EXP_FP16
                    const float s = s0[i] - max;
                    const float val = expf(s);
#else
                    lm_ggml_fp16_t s = LM_GGML_FP32_TO_FP16(s0[i] - max);
                    memcpy(&scvt, &s, sizeof(scvt));
                    const float val = LM_GGML_FP16_TO_FP32(table_exp_f16[scvt]);
#endif
                    sum += (lm_ggml_float)val;
                    ds0[i] = val;
                }
            }

            assert(sum > 0.0);
            sum = (1.0 - eps)/sum;
        }

        // grad(src0) = (softmax(src0) - src1) * grad(cross_entropy_loss(src0, src1)) / nr
        lm_ggml_vec_scale_f32(nc, ds0, sum);
        lm_ggml_vec_add1_f32(nc, ds0, ds0, eps);
        lm_ggml_vec_sub_f32(nc, ds0, ds0, s1);
        lm_ggml_vec_scale_f32(nc, ds0, d[0] / (float) nr);

#ifndef NDEBUG
        for (int i = 0; i < nc; ++i) {
            assert(!isnan(ds0[i]));
            assert(!isinf(ds0[i]));
        }
#endif
    }
}

static void lm_ggml_compute_forward_cross_entropy_loss_back(
        const struct lm_ggml_compute_params * params,
        const struct lm_ggml_tensor * src0,
        const struct lm_ggml_tensor * src1,
        const struct lm_ggml_tensor * opt0,
        struct lm_ggml_tensor * dst) {
    switch (src0->type) {
        case LM_GGML_TYPE_F32:
            {
                lm_ggml_compute_forward_cross_entropy_loss_back_f32(params, src0, src1, opt0, dst);
            } break;
        default:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

/////////////////////////////////

static void lm_ggml_compute_forward(struct lm_ggml_compute_params * params, struct lm_ggml_tensor * tensor) {
    LM_GGML_ASSERT(params);

    if (tensor->op == LM_GGML_OP_NONE) {
        return;
    }

#ifdef LM_GGML_USE_CUBLAS
    bool skip_cpu = lm_ggml_cuda_compute_forward(params, tensor);
    if (skip_cpu) {
        return;
    }
    LM_GGML_ASSERT(tensor->src[0] == NULL || tensor->src[0]->backend == LM_GGML_BACKEND_CPU);
    LM_GGML_ASSERT(tensor->src[1] == NULL || tensor->src[1]->backend == LM_GGML_BACKEND_CPU);
#endif // LM_GGML_USE_CUBLAS

    switch (tensor->op) {
        case LM_GGML_OP_DUP:
            {
                lm_ggml_compute_forward_dup(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_ADD:
            {
                lm_ggml_compute_forward_add(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_ADD1:
            {
                lm_ggml_compute_forward_add1(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_ACC:
            {
                lm_ggml_compute_forward_acc(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_SUB:
            {
                lm_ggml_compute_forward_sub(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_MUL:
            {
                lm_ggml_compute_forward_mul(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_DIV:
            {
                lm_ggml_compute_forward_div(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_SQR:
            {
                lm_ggml_compute_forward_sqr(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_SQRT:
            {
                lm_ggml_compute_forward_sqrt(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_LOG:
            {
                lm_ggml_compute_forward_log(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_SUM:
            {
                lm_ggml_compute_forward_sum(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_SUM_ROWS:
            {
                lm_ggml_compute_forward_sum_rows(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_MEAN:
            {
                lm_ggml_compute_forward_mean(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_ARGMAX:
            {
                lm_ggml_compute_forward_argmax(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_REPEAT:
            {
                lm_ggml_compute_forward_repeat(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_REPEAT_BACK:
            {
                lm_ggml_compute_forward_repeat_back(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_CONCAT:
            {
                lm_ggml_compute_forward_concat(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_SILU_BACK:
            {
                lm_ggml_compute_forward_silu_back(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_NORM:
            {
                lm_ggml_compute_forward_norm(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_RMS_NORM:
            {
                lm_ggml_compute_forward_rms_norm(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_RMS_NORM_BACK:
            {
                lm_ggml_compute_forward_rms_norm_back(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_GROUP_NORM:
            {
                lm_ggml_compute_forward_group_norm(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_MUL_MAT:
            {
                lm_ggml_compute_forward_mul_mat(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_OUT_PROD:
            {
                lm_ggml_compute_forward_out_prod(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_SCALE:
            {
                lm_ggml_compute_forward_scale(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_SET:
            {
                lm_ggml_compute_forward_set(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_CPY:
            {
                lm_ggml_compute_forward_cpy(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_CONT:
            {
                lm_ggml_compute_forward_cont(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_RESHAPE:
            {
                lm_ggml_compute_forward_reshape(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_VIEW:
            {
                lm_ggml_compute_forward_view(params, tensor->src[0]);
            } break;
        case LM_GGML_OP_PERMUTE:
            {
                lm_ggml_compute_forward_permute(params, tensor->src[0]);
            } break;
        case LM_GGML_OP_TRANSPOSE:
            {
                lm_ggml_compute_forward_transpose(params, tensor->src[0]);
            } break;
        case LM_GGML_OP_GET_ROWS:
            {
                lm_ggml_compute_forward_get_rows(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_GET_ROWS_BACK:
            {
                lm_ggml_compute_forward_get_rows_back(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_DIAG:
            {
                lm_ggml_compute_forward_diag(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_DIAG_MASK_INF:
            {
                lm_ggml_compute_forward_diag_mask_inf(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_DIAG_MASK_ZERO:
            {
                lm_ggml_compute_forward_diag_mask_zero(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_SOFT_MAX:
            {
                lm_ggml_compute_forward_soft_max(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_SOFT_MAX_BACK:
            {
                lm_ggml_compute_forward_soft_max_back(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_ROPE:
            {
                lm_ggml_compute_forward_rope(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_ROPE_BACK:
            {
                lm_ggml_compute_forward_rope_back(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_ALIBI:
            {
                lm_ggml_compute_forward_alibi(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_CLAMP:
            {
                lm_ggml_compute_forward_clamp(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_CONV_1D:
            {
                lm_ggml_compute_forward_conv_1d(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_CONV_1D_STAGE_0:
            {
                lm_ggml_compute_forward_conv_1d_stage_0(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_CONV_1D_STAGE_1:
            {
                lm_ggml_compute_forward_conv_1d_stage_1(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_CONV_TRANSPOSE_1D:
            {
                lm_ggml_compute_forward_conv_transpose_1d(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_CONV_2D:
            {
                lm_ggml_compute_forward_conv_2d(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_CONV_2D_STAGE_0:
            {
                lm_ggml_compute_forward_conv_2d_stage_0(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_CONV_2D_STAGE_1:
            {
                lm_ggml_compute_forward_conv_2d_stage_1(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_CONV_TRANSPOSE_2D:
            {
                lm_ggml_compute_forward_conv_transpose_2d(params, tensor->src[0], tensor->src[1], tensor);
            } break;
        case LM_GGML_OP_POOL_1D:
            {
                lm_ggml_compute_forward_pool_1d(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_POOL_2D:
            {
                lm_ggml_compute_forward_pool_2d(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_UPSCALE:
            {
                lm_ggml_compute_forward_upscale(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_FLASH_ATTN:
            {
                const int32_t t = lm_ggml_get_op_params_i32(tensor, 0);
                LM_GGML_ASSERT(t == 0 || t == 1);
                const bool masked = t != 0;
                lm_ggml_compute_forward_flash_attn(params, tensor->src[0], tensor->src[1], tensor->src[2], masked, tensor);
            } break;
        case LM_GGML_OP_FLASH_FF:
            {
                lm_ggml_compute_forward_flash_ff(params, tensor->src[0], tensor->src[1], tensor->src[2], tensor->src[3], tensor->src[4], tensor);
            } break;
        case LM_GGML_OP_FLASH_ATTN_BACK:
            {
                int32_t t = lm_ggml_get_op_params_i32(tensor, 0);
                LM_GGML_ASSERT(t == 0 || t == 1);
                bool masked = t != 0;
                lm_ggml_compute_forward_flash_attn_back(params, tensor->src[0], tensor->src[1], tensor->src[2], tensor->src[3], masked, tensor);
            } break;
        case LM_GGML_OP_WIN_PART:
            {
                lm_ggml_compute_forward_win_part(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_WIN_UNPART:
            {
                lm_ggml_compute_forward_win_unpart(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_UNARY:
            {
                lm_ggml_compute_forward_unary(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_GET_REL_POS:
            {
                lm_ggml_compute_forward_get_rel_pos(params, tensor->src[0], tensor);
            } break;
        case LM_GGML_OP_ADD_REL_POS:
            {
                lm_ggml_compute_forward_add_rel_pos(params, tensor->src[0], tensor->src[1], tensor->src[2], tensor);
            } break;
        case LM_GGML_OP_MAP_UNARY:
            {
                lm_ggml_unary_op_f32_t fun;
                memcpy(&fun, tensor->op_params, sizeof(fun));
                lm_ggml_compute_forward_map_unary(params, tensor->src[0], tensor, fun);
            }
            break;
        case LM_GGML_OP_MAP_BINARY:
            {
                lm_ggml_binary_op_f32_t fun;
                memcpy(&fun, tensor->op_params, sizeof(fun));
                lm_ggml_compute_forward_map_binary(params, tensor->src[0], tensor->src[1], tensor, fun);
            }
            break;
        case LM_GGML_OP_MAP_CUSTOM1_F32:
            {
                lm_ggml_custom1_op_f32_t fun;
                memcpy(&fun, tensor->op_params, sizeof(fun));
                lm_ggml_compute_forward_map_custom1_f32(params, tensor->src[0], tensor, fun);
            }
            break;
        case LM_GGML_OP_MAP_CUSTOM2_F32:
            {
                lm_ggml_custom2_op_f32_t fun;
                memcpy(&fun, tensor->op_params, sizeof(fun));
                lm_ggml_compute_forward_map_custom2_f32(params, tensor->src[0], tensor->src[1], tensor, fun);
            }
            break;
        case LM_GGML_OP_MAP_CUSTOM3_F32:
            {
                lm_ggml_custom3_op_f32_t fun;
                memcpy(&fun, tensor->op_params, sizeof(fun));
                lm_ggml_compute_forward_map_custom3_f32(params, tensor->src[0], tensor->src[1], tensor->src[2], tensor, fun);
            }
            break;
        case LM_GGML_OP_MAP_CUSTOM1:
            {
                lm_ggml_compute_forward_map_custom1(params, tensor->src[0], tensor);
            }
            break;
        case LM_GGML_OP_MAP_CUSTOM2:
            {
                lm_ggml_compute_forward_map_custom2(params, tensor->src[0], tensor->src[1], tensor);
            }
            break;
        case LM_GGML_OP_MAP_CUSTOM3:
            {
                lm_ggml_compute_forward_map_custom3(params, tensor->src[0], tensor->src[1], tensor->src[2], tensor);
            }
            break;
        case LM_GGML_OP_CROSS_ENTROPY_LOSS:
            {
                lm_ggml_compute_forward_cross_entropy_loss(params, tensor->src[0], tensor->src[1], tensor);
            }
            break;
        case LM_GGML_OP_CROSS_ENTROPY_LOSS_BACK:
            {
                lm_ggml_compute_forward_cross_entropy_loss_back(params, tensor->src[0], tensor->src[1], tensor->src[2], tensor);
            }
            break;
        case LM_GGML_OP_NONE:
            {
                // nop
            } break;
        case LM_GGML_OP_COUNT:
            {
                LM_GGML_ASSERT(false);
            } break;
    }
}

////////////////////////////////////////////////////////////////////////////////

static_assert(LM_GGML_GRAPH_HASHTABLE_SIZE > LM_GGML_MAX_NODES * 2, "LM_GGML_GRAPH_HT_SIZE is too small");

static size_t hash(void * p) {
    return (size_t)p % LM_GGML_GRAPH_HASHTABLE_SIZE;
}

static size_t hash_find(void * hash_table[], void * p) {
    size_t h = hash(p);

    // linear probing
    size_t i = h;
    while (hash_table[i] != NULL && hash_table[i] != p) {
        i = (i + 1) % LM_GGML_GRAPH_HASHTABLE_SIZE;
        if (i == h) {
            // visited all hash table entries -> not found
            return LM_GGML_GRAPH_HASHTABLE_SIZE;
        }
    }
    return i;
}

static bool hash_insert(void * hash_table[], void * p) {
    size_t i = hash_find(hash_table, p);

    LM_GGML_ASSERT(i < LM_GGML_GRAPH_HASHTABLE_SIZE); // assert that not full

    if (hash_table[i] == p) {
        return true;
    }

    // insert
    LM_GGML_ASSERT(hash_table[i] == NULL);
    hash_table[i] = p;
    return false;
}

static bool hash_contains(void * hash_table[], void * p) {
    size_t i = hash_find(hash_table, p);
    return (i < LM_GGML_GRAPH_HASHTABLE_SIZE) && (hash_table[i] == p);
}

struct hash_map {
    void * keys[LM_GGML_GRAPH_HASHTABLE_SIZE];
    void * vals[LM_GGML_GRAPH_HASHTABLE_SIZE];
};

static struct hash_map * new_hash_map(void) {
    struct hash_map * result = malloc(sizeof(struct hash_map));
    for (int i=0; i<LM_GGML_GRAPH_HASHTABLE_SIZE; ++i) {
        result->keys[i] = NULL;
        result->vals[i] = NULL;
    }
    return result;
}

static void free_hash_map(struct hash_map * map) {
    free(map);
}

// gradient checkpointing

static struct lm_ggml_tensor * lm_ggml_recompute_graph_node(
        struct lm_ggml_context * ctx,
        struct lm_ggml_cgraph  * graph,
        struct hash_map     * replacements,
        struct lm_ggml_tensor  * node) {

    if (node == NULL) {
        return NULL;
    }

    if (node->is_param) {
        return node;
    }

    if (!hash_contains(graph->visited_hash_table, node)) {
        return node;
    }

    int count_children = 0;
    for (int k = 0; k < LM_GGML_MAX_SRC; ++k) {
        if (node->src[k]) {
            ++count_children;
        }
    }

    if (count_children == 0) {
        return node;
    }

    size_t i = hash_find(replacements->keys, node);
    LM_GGML_ASSERT(i < LM_GGML_GRAPH_HASHTABLE_SIZE); // assert that not full
    if (replacements->keys[i] == node) {
        return (struct lm_ggml_tensor *) replacements->vals[i];
    }

    struct lm_ggml_tensor * clone = lm_ggml_new_tensor(ctx, node->type, node->n_dims, node->ne);

    // insert clone into replacements
    LM_GGML_ASSERT(replacements->keys[i] == NULL); // assert that we don't overwrite
    replacements->keys[i] = node;
    replacements->vals[i] = clone;

    clone->op       = node->op;
    clone->grad     = node->grad;
    clone->is_param = node->is_param;
    clone->extra    = node->extra;
    for (int k = 0; k < LM_GGML_MAX_DIMS; ++k) {
        clone->nb[k] = node->nb[k];
    }
    for (int k = 0; k < LM_GGML_MAX_SRC; ++k) {
        clone->src[k] = lm_ggml_recompute_graph_node(ctx, graph, replacements, node->src[k]);
    }
    if (node->view_src != NULL) {
        clone->data = (node->view_src->data == NULL)
                        ? NULL // view_src not yet allocated
                        : (char *) node->view_src->data // view_src already allocated
                                 + node->view_offs;
        clone->view_src  = node->view_src;
        clone->view_offs = node->view_offs;
    }

    LM_GGML_ASSERT(sizeof(node->op_params) == sizeof(int32_t) * (LM_GGML_MAX_OP_PARAMS / sizeof(int32_t)));
    LM_GGML_ASSERT(sizeof(node->name)      == LM_GGML_MAX_NAME);
    memcpy(clone->op_params, node->op_params, sizeof(node->op_params));
    lm_ggml_format_name(clone, "%s (clone)", lm_ggml_get_name(node));

    return clone;
}

void lm_ggml_build_backward_gradient_checkpointing(
        struct lm_ggml_context   * ctx,
        struct lm_ggml_cgraph    * gf,
        struct lm_ggml_cgraph    * gb,
        struct lm_ggml_cgraph    * gb_tmp,
        struct lm_ggml_tensor  * * checkpoints,
        int                     n_checkpoints) {
    *gb_tmp = *gf;
    lm_ggml_build_backward_expand(ctx, gf, gb_tmp, true);

    if (n_checkpoints <= 0) {
        *gb = *gb_tmp;
        return;
    }

    struct hash_map * replacements = new_hash_map();

    // insert checkpoints in replacements
    for (int i = 0; i < n_checkpoints; ++i) {
        size_t k = hash_find(replacements->keys, checkpoints[i]);
        LM_GGML_ASSERT(k < LM_GGML_GRAPH_HASHTABLE_SIZE); // assert that not full
        LM_GGML_ASSERT(replacements->keys[k] == NULL); // assert that we don't overwrite
        replacements->keys[k] = checkpoints[i];
        replacements->vals[k] = checkpoints[i];
    }

    *gb = *gf;
    // rewrite gb_tmp->nodes[gf->n_nodes:gb_tmp->n_nodes],
    // replacing references to gb_tmp->nodes[0:gf->n_nodes] ( == gf->nodes[0:gf->n_nodes]),
    // by recomputing them from checkpoints
    for (int i = gf->n_nodes; i<gb_tmp->n_nodes; ++i) {
        struct lm_ggml_tensor * node = gb_tmp->nodes[i];
        for (int k = 0; k < LM_GGML_MAX_SRC; ++k) {
            // insert new tensors recomputing src, reusing already made replacements,
            // remember replacements: remember new tensors with mapping from corresponding gf nodes
            // recurse for input tensors,
            // unless (i.e. terminating when) input tensors are replacments (like checkpoints)
            node->src[k] = lm_ggml_recompute_graph_node(ctx, gf, replacements, node->src[k]);
        }
        // insert rewritten backward node with replacements made into resulting backward graph gb
        lm_ggml_build_forward_expand(gb, node);
    }

    free_hash_map(replacements);
}

// functions to change gradients considering the case that input a might be initial gradient with zero value

static struct lm_ggml_tensor * lm_ggml_add_or_set(struct lm_ggml_context * ctx, struct lm_ggml_tensor * a, struct lm_ggml_tensor * b, void * zero_table[]) {
    if (hash_contains(zero_table, a)) {
        return b;
    } else {
        return lm_ggml_add_impl(ctx, a, b, false);
    }
}

static struct lm_ggml_tensor * lm_ggml_acc_or_set(struct lm_ggml_context * ctx, struct lm_ggml_tensor * a, struct lm_ggml_tensor * b, size_t nb1, size_t nb2, size_t nb3, size_t offset, void * zero_table[]) {
    if (hash_contains(zero_table, a)) {
        struct lm_ggml_tensor * a_zero = lm_ggml_scale(ctx, a, lm_ggml_new_f32(ctx, 0));
        return lm_ggml_acc_impl(ctx, a_zero, b, nb1, nb2, nb3, offset, false);
    } else {
        return lm_ggml_acc_impl(ctx, a, b, nb1, nb2, nb3, offset, false);
    }
}

static struct lm_ggml_tensor * lm_ggml_add1_or_set(struct lm_ggml_context * ctx, struct lm_ggml_tensor * a, struct lm_ggml_tensor * b, void * zero_table[]) {
    if (hash_contains(zero_table, a)) {
        return lm_ggml_repeat(ctx, b, a);
    } else {
        return lm_ggml_add1_impl(ctx, a, b, false);
    }
}

static struct lm_ggml_tensor * lm_ggml_sub_or_set(struct lm_ggml_context * ctx, struct lm_ggml_tensor * a, struct lm_ggml_tensor * b, void * zero_table[]) {
    if (hash_contains(zero_table, a)) {
        return lm_ggml_neg(ctx, b);
    } else {
        return lm_ggml_sub_impl(ctx, a, b, false);
    }
}

static void lm_ggml_compute_backward(struct lm_ggml_context * ctx, struct lm_ggml_tensor * tensor, void * zero_table[]) {
    struct lm_ggml_tensor * src0 = tensor->src[0];
    struct lm_ggml_tensor * src1 = tensor->src[1];

    switch (tensor->op) {
        case LM_GGML_OP_DUP:
            {
                if (src0->grad) {
                    src0->grad = lm_ggml_add_or_set(ctx, src0->grad, tensor->grad, zero_table);
                }
            } break;
        case LM_GGML_OP_ADD:
            {
                if (src0->grad) {
                    src0->grad = lm_ggml_add_or_set(ctx, src0->grad, tensor->grad, zero_table);
                }
                if (src1->grad) {
                    src1->grad = lm_ggml_add_or_set(ctx, src1->grad, tensor->grad, zero_table);
                }
            } break;
        case LM_GGML_OP_ADD1:
            {
                if (src0->grad) {
                    src0->grad = lm_ggml_add_or_set(ctx, src0->grad, tensor->grad, zero_table);
                }
                if (src1->grad) {
                    src1->grad = lm_ggml_add_or_set(ctx,
                        src1->grad,
                        lm_ggml_mean(ctx, tensor->grad), // TODO: should probably be sum instead of mean
                        zero_table);
                }
            } break;
        case LM_GGML_OP_ACC:
            {
                if (src0->grad) {
                    src0->grad = lm_ggml_add_or_set(ctx, src0->grad, tensor->grad, zero_table);
                }
                if (src1->grad) {
                    const size_t nb1     = ((int32_t *) tensor->op_params)[0];
                    const size_t nb2     = ((int32_t *) tensor->op_params)[1];
                    const size_t nb3     = ((int32_t *) tensor->op_params)[2];
                    const size_t offset  = ((int32_t *) tensor->op_params)[3];

                    struct lm_ggml_tensor * tensor_grad_view = lm_ggml_view_4d(ctx,
                        tensor->grad,
                        src1->grad->ne[0],
                        src1->grad->ne[1],
                        src1->grad->ne[2],
                        src1->grad->ne[3],
                        nb1, nb2, nb3, offset);

                    src1->grad =
                        lm_ggml_add_or_set(ctx,
                            src1->grad,
                            lm_ggml_reshape(ctx,
                                lm_ggml_cont(ctx, tensor_grad_view),
                                src1->grad),
                            zero_table);
                }
            } break;
        case LM_GGML_OP_SUB:
            {
                if (src0->grad) {
                    src0->grad = lm_ggml_add_or_set(ctx, src0->grad, tensor->grad, zero_table);
                }
                if (src1->grad) {
                    src1->grad = lm_ggml_sub_or_set(ctx, src1->grad, tensor->grad, zero_table);
                }
            } break;
        case LM_GGML_OP_MUL:
            {
                if (src0->grad) {
                    src0->grad =
                        lm_ggml_add_or_set(ctx,
                                src0->grad,
                                lm_ggml_mul(ctx, src1, tensor->grad),
                                zero_table);
                }
                if (src1->grad) {
                    src1->grad =
                        lm_ggml_add_or_set(ctx,
                                src1->grad,
                                lm_ggml_mul(ctx, src0, tensor->grad),
                                zero_table);
                }
            } break;
        case LM_GGML_OP_DIV:
            {
                if (src0->grad) {
                    src0->grad =
                        lm_ggml_add_or_set(ctx,
                                src0->grad,
                                lm_ggml_div(ctx, tensor->grad, src1),
                                zero_table);
                }
                if (src1->grad) {
                    src1->grad =
                        lm_ggml_sub_or_set(ctx,
                                src1->grad,
                                lm_ggml_mul(ctx,
                                    tensor->grad,
                                    lm_ggml_div(ctx, tensor, src1)),
                                zero_table);
                }
            } break;
        case LM_GGML_OP_SQR:
            {
                if (src0->grad) {
                    src0->grad =
                        lm_ggml_add_or_set(ctx,
                                src0->grad,
                                lm_ggml_scale(ctx,
                                    lm_ggml_mul(ctx, src0, tensor->grad),
                                    lm_ggml_new_f32(ctx, 2.0f)),
                                zero_table);
                }
            } break;
        case LM_GGML_OP_SQRT:
            {
                if (src0->grad) {
                    src0->grad =
                        lm_ggml_add_or_set(ctx,
                                src0->grad,
                                lm_ggml_scale(ctx,
                                    lm_ggml_div(ctx,
                                        tensor->grad,
                                        tensor),
                                    lm_ggml_new_f32(ctx, 0.5f)),
                                zero_table);
                }
            } break;
        case LM_GGML_OP_LOG:
            {
                if (src0->grad) {
                    src0->grad =
                        lm_ggml_add_or_set(ctx,
                                src0->grad,
                                lm_ggml_div(ctx,
                                    tensor->grad,
                                    src0),
                                zero_table);
                }
            } break;
        case LM_GGML_OP_SUM:
            {
                if (src0->grad) {
                    src0->grad =
                        lm_ggml_add1_or_set(ctx,
                                src0->grad,
                                tensor->grad,
                                zero_table);
                }
            } break;
        case LM_GGML_OP_SUM_ROWS:
            {
                if (src0->grad) {
                    src0->grad =
                        lm_ggml_add_or_set(ctx,
                                src0->grad,
                                lm_ggml_repeat(ctx,
                                    tensor->grad,
                                    src0->grad),
                                zero_table);
                }
            } break;
        case LM_GGML_OP_MEAN:
        case LM_GGML_OP_ARGMAX:
            {
                LM_GGML_ASSERT(false); // TODO: implement
            } break;
        case LM_GGML_OP_REPEAT:
            {
                // necessary for llama
                if (src0->grad) {
                    src0->grad = lm_ggml_add_or_set(ctx,
                            src0->grad,
                            lm_ggml_repeat_back(ctx, tensor->grad, src0->grad),
                            zero_table);
                }
            } break;
        case LM_GGML_OP_REPEAT_BACK:
            {
                if (src0->grad) {
                    // TODO: test this
                    src0->grad = lm_ggml_add_or_set(ctx,
                            src0->grad,
                            lm_ggml_repeat(ctx, tensor->grad, src0->grad),
                            zero_table);
                }
            } break;
        case LM_GGML_OP_CONCAT:
            {
                LM_GGML_ASSERT(false); // TODO: implement
            } break;
        case LM_GGML_OP_SILU_BACK:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_NORM:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_RMS_NORM:
            {
                // necessary for llama
                if (src0->grad) {
                    float eps;
                    memcpy(&eps, tensor->op_params, sizeof(float));

                    src0->grad = lm_ggml_add_or_set(ctx,
                            src0->grad,
                            lm_ggml_rms_norm_back(ctx, src0, tensor->grad, eps),
                            zero_table);
                }
            } break;
        case LM_GGML_OP_RMS_NORM_BACK:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_GROUP_NORM:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_MUL_MAT:
            {
                // https://cs231n.github.io/optimization-2/#staged
                // # forward pass
                // s0 = np.random.randn(5, 10)
                // s1 = np.random.randn(10, 3)
                // t = s0.dot(s1)

                // # now suppose we had the gradient on t from above in the circuit
                // dt = np.random.randn(*t.shape) # same shape as t
                // ds0 = dt.dot(s1.T) #.T gives the transpose of the matrix
                // ds1 = t.T.dot(dt)

                // tensor.shape [m,p,qq,rr]
                // src0.shape   [n,m,q1,r1]
                // src1.shape   [n,p,qq,rr]

                // necessary for llama
                if (src0->grad) {
                    struct lm_ggml_tensor * s1_tg =
                        lm_ggml_out_prod(ctx, // [n,m,qq,rr]
                            src1,          // [n,p,qq,rr]
                            tensor->grad); // [m,p,qq,rr]
                    const int64_t qq = s1_tg->ne[2];
                    const int64_t rr = s1_tg->ne[3];
                    const int64_t q1 = src0->ne[2];
                    const int64_t r1 = src0->ne[3];
                    const bool ne2_broadcasted = qq > q1;
                    const bool ne3_broadcasted = rr > r1;
                    if (ne2_broadcasted || ne3_broadcasted) {
                        // sum broadcast repetitions of s1_tg into shape of src0
                        s1_tg = lm_ggml_repeat_back(ctx, s1_tg, src0);
                    }
                    src0->grad =
                        lm_ggml_add_or_set(ctx,
                                src0->grad, // [n,m,q1,r1]
                                s1_tg,      // [n,m,q1,r1]
                                zero_table);
                }
                if (src1->grad) {
                    src1->grad =
                        lm_ggml_add_or_set(ctx,
                                src1->grad,                            // [n,p,qq,rr]
                                // lm_ggml_mul_mat(ctx,                   // [n,p,qq,rr]
                                //     lm_ggml_cont(ctx,                  // [m,n,q1,r1]
                                //         lm_ggml_transpose(ctx, src0)), // [m,n,q1,r1]
                                //     tensor->grad),                  // [m,p,qq,rr]

                                // // when src0 is bigger than tensor->grad (this is mostly the case in llama),
                                // // avoid transpose of src0, rather transpose smaller tensor->grad
                                // // and then use lm_ggml_out_prod
                                lm_ggml_out_prod(ctx,                  // [n,p,qq,rr]
                                    src0,                           // [n,m,q1,r1]
                                    lm_ggml_transpose(ctx,             // [p,m,qq,rr]
                                        tensor->grad)),             // [m,p,qq,rr]
                                zero_table);
                }
            } break;
        case LM_GGML_OP_OUT_PROD:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_SCALE:
            {
                // necessary for llama
                if (src0->grad) {
                    src0->grad =
                        lm_ggml_add_or_set(ctx,
                            src0->grad,
                            lm_ggml_scale_impl(ctx, tensor->grad, src1, false),
                            zero_table);
                }
                if (src1->grad) {
                    src1->grad =
                        lm_ggml_add_or_set(ctx,
                            src1->grad,
                            lm_ggml_sum(ctx, lm_ggml_mul_impl(ctx, tensor->grad, src0, false)),
                            zero_table);
                }
            } break;
        case LM_GGML_OP_SET:
            {
                const size_t nb1     = ((int32_t *) tensor->op_params)[0];
                const size_t nb2     = ((int32_t *) tensor->op_params)[1];
                const size_t nb3     = ((int32_t *) tensor->op_params)[2];
                const size_t offset  = ((int32_t *) tensor->op_params)[3];

                struct lm_ggml_tensor * tensor_grad_view = NULL;

                if (src0->grad || src1->grad) {
                    LM_GGML_ASSERT(src0->type == tensor->type);
                    LM_GGML_ASSERT(tensor->grad->type == tensor->type);
                    LM_GGML_ASSERT(tensor->grad->type == src1->grad->type);

                    tensor_grad_view = lm_ggml_view_4d(ctx,
                        tensor->grad,
                        src1->grad->ne[0],
                        src1->grad->ne[1],
                        src1->grad->ne[2],
                        src1->grad->ne[3],
                        nb1, nb2, nb3, offset);
                }

                if (src0->grad) {
                    src0->grad = lm_ggml_add_or_set(ctx,
                        src0->grad,
                        lm_ggml_acc_impl(ctx,
                            tensor->grad,
                            lm_ggml_neg(ctx, tensor_grad_view),
                            nb1, nb2, nb3, offset, false),
                        zero_table);
                }

                if (src1->grad) {
                    src1->grad =
                        lm_ggml_add_or_set(ctx,
                            src1->grad,
                            lm_ggml_reshape(ctx,
                                lm_ggml_cont(ctx, tensor_grad_view),
                                src1->grad),
                            zero_table);
                }
            } break;
        case LM_GGML_OP_CPY:
            {
                // necessary for llama
                // cpy overwrites value of src1 by src0 and returns view(src1)
                // the overwriting is mathematically equivalent to:
                // tensor = src0 * 1 + src1 * 0
                if (src0->grad) {
                    // dsrc0 = dtensor * 1
                    src0->grad = lm_ggml_add_or_set(ctx, src0->grad, tensor->grad, zero_table);
                }
                if (src1->grad) {
                    // dsrc1 = dtensor * 0 -> noop
                }
            } break;
        case LM_GGML_OP_CONT:
            {
                // same as cpy
                if (src0->grad) {
                    LM_GGML_ASSERT(lm_ggml_is_contiguous(src0->grad));
                    LM_GGML_ASSERT(lm_ggml_is_contiguous(tensor->grad));
                    src0->grad = lm_ggml_add_or_set(ctx, src0->grad, tensor->grad, zero_table);
                }
            } break;
        case LM_GGML_OP_RESHAPE:
            {
                // necessary for llama
                if (src0->grad) {
                    src0->grad =
                        lm_ggml_add_or_set(ctx, src0->grad,
                            lm_ggml_reshape(ctx,
                                lm_ggml_is_contiguous(tensor->grad)
                                    ? tensor->grad
                                    : lm_ggml_cont(ctx, tensor->grad),
                                src0->grad),
                        zero_table);
                }
            } break;
        case LM_GGML_OP_VIEW:
            {
                // necessary for llama
                if (src0->grad) {
                    size_t offset;

                    memcpy(&offset, tensor->op_params, sizeof(offset));

                    size_t nb1     = tensor->nb[1];
                    size_t nb2     = tensor->nb[2];
                    size_t nb3     = tensor->nb[3];

                    if (src0->type != src0->grad->type) {
                        // gradient is typically F32, but src0 could be other type
                        size_t ng = lm_ggml_element_size(src0->grad);
                        size_t n0 = lm_ggml_element_size(src0);
                        LM_GGML_ASSERT(offset % n0 == 0);
                        LM_GGML_ASSERT(nb1 % n0 == 0);
                        LM_GGML_ASSERT(nb2 % n0 == 0);
                        LM_GGML_ASSERT(nb3 % n0 == 0);
                        offset = (offset / n0) * ng;
                        nb1 = (nb1 / n0) * ng;
                        nb2 = (nb2 / n0) * ng;
                        nb3 = (nb3 / n0) * ng;
                    }

                    src0->grad = lm_ggml_acc_or_set(ctx, src0->grad, tensor->grad, nb1, nb2, nb3, offset, zero_table);
                }
            } break;
        case LM_GGML_OP_PERMUTE:
            {
                // necessary for llama
                if (src0->grad) {
                    int32_t * axes = (int32_t *) tensor->op_params;
                    int axis0 = axes[0] & 0x3;
                    int axis1 = axes[1] & 0x3;
                    int axis2 = axes[2] & 0x3;
                    int axis3 = axes[3] & 0x3;
                    int axes_backward[4] = {0,0,0,0};
                    axes_backward[axis0] = 0;
                    axes_backward[axis1] = 1;
                    axes_backward[axis2] = 2;
                    axes_backward[axis3] = 3;
                    src0->grad =
                        lm_ggml_add_or_set(ctx, src0->grad,
                            lm_ggml_permute(ctx,
                                tensor->grad,
                                axes_backward[0],
                                axes_backward[1],
                                axes_backward[2],
                                axes_backward[3]),
                            zero_table);
                }
            } break;
        case LM_GGML_OP_TRANSPOSE:
            {
                // necessary for llama
                if (src0->grad) {
                    src0->grad =
                        lm_ggml_add_or_set(ctx, src0->grad,
                            lm_ggml_transpose(ctx, tensor->grad),
                        zero_table);
                }
            } break;
        case LM_GGML_OP_GET_ROWS:
            {
                // necessary for llama (only for tokenizer)
                if (src0->grad) {
                    src0->grad =
                        lm_ggml_add_or_set(ctx, src0->grad,
                            // last lm_ggml_get_rows_back argument src0->grad is only
                            // necessary to setup correct output shape
                            lm_ggml_get_rows_back(ctx, tensor->grad, src1, src0->grad),
                        zero_table);
                }
                if (src1->grad) {
                    // noop
                }
            } break;
        case LM_GGML_OP_GET_ROWS_BACK:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_DIAG:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_DIAG_MASK_INF:
            {
                // necessary for llama
                if (src0->grad) {
                    const int n_past = ((int32_t *) tensor->op_params)[0];
                    src0->grad =
                        lm_ggml_add_or_set(ctx, src0->grad,
                            lm_ggml_diag_mask_zero_impl(ctx, tensor->grad, n_past, false),
                        zero_table);
                }
            } break;
        case LM_GGML_OP_DIAG_MASK_ZERO:
            {
                // necessary for llama
                if (src0->grad) {
                    const int n_past = ((int32_t *) tensor->op_params)[0];
                    src0->grad =
                        lm_ggml_add_or_set(ctx, src0->grad,
                            lm_ggml_diag_mask_zero_impl(ctx, tensor->grad, n_past, false),
                        zero_table);
                }
            } break;
        case LM_GGML_OP_SOFT_MAX:
            {
                // necessary for llama
                if (src0->grad) {
                    src0->grad =
                        lm_ggml_add_or_set(ctx, src0->grad,
                            lm_ggml_soft_max_back(ctx, tensor->grad, tensor),
                        zero_table);
                }

            } break;
        case LM_GGML_OP_SOFT_MAX_BACK:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_ROPE:
            {
                // necessary for llama
                if (src0->grad) {
                    //const int n_past = ((int32_t *) tensor->op_params)[0];
                    const int n_dims = ((int32_t *) tensor->op_params)[1];
                    const int mode   = ((int32_t *) tensor->op_params)[2];
                    const int n_ctx  = ((int32_t *) tensor->op_params)[3];
                    float freq_base;
                    float freq_scale;
                    float xpos_base;
                    bool  xpos_down;
                    memcpy(&freq_base,  (int32_t *) tensor->op_params + 4, sizeof(float));
                    memcpy(&freq_scale, (int32_t *) tensor->op_params + 5, sizeof(float));
                    memcpy(&xpos_base,  (int32_t *) tensor->op_params + 6, sizeof(float));
                    memcpy(&xpos_down,  (int32_t *) tensor->op_params + 7, sizeof(bool));

                    src0->grad = lm_ggml_add_or_set(ctx,
                            src0->grad,
                            lm_ggml_rope_back(ctx,
                                tensor->grad,
                                src1,
                                n_dims,
                                mode,
                                n_ctx,
                                freq_base,
                                freq_scale,
                                xpos_base,
                                xpos_down),
                            zero_table);
                }
            } break;
        case LM_GGML_OP_ROPE_BACK:
            {
                if (src0->grad) {
                    //const int n_past = ((int32_t *) tensor->op_params)[0];
                    const int n_dims = ((int32_t *) tensor->op_params)[1];
                    const int mode   = ((int32_t *) tensor->op_params)[2];
                    const int n_ctx  = ((int32_t *) tensor->op_params)[3];
                    float freq_base;
                    float freq_scale;
                    float xpos_base;
                    bool  xpos_down;
                    memcpy(&freq_base,  (int32_t *) tensor->op_params + 4, sizeof(float));
                    memcpy(&freq_scale, (int32_t *) tensor->op_params + 5, sizeof(float));
                    memcpy(&xpos_base,  (int32_t *) tensor->op_params + 6, sizeof(float));
                    memcpy(&xpos_down,  (int32_t *) tensor->op_params + 7, sizeof(bool));

                    src0->grad = lm_ggml_add_or_set(ctx,
                            src0->grad,
                            lm_ggml_rope_impl(ctx,
                                tensor->grad,
                                src1,
                                n_dims,
                                mode,
                                n_ctx,
                                freq_base,
                                freq_scale,
                                xpos_base,
                                xpos_down,
                                false),
                            zero_table);
                }
            } break;
        case LM_GGML_OP_ALIBI:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_CLAMP:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_CONV_1D:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_CONV_1D_STAGE_0:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_CONV_1D_STAGE_1:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_CONV_TRANSPOSE_1D:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_CONV_2D:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_CONV_2D_STAGE_0:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_CONV_2D_STAGE_1:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_CONV_TRANSPOSE_2D:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_POOL_1D:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_POOL_2D:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_UPSCALE:
            {
                LM_GGML_ASSERT(false); // TODO: not implemented
            } break;
        case LM_GGML_OP_FLASH_ATTN:
            {
                struct lm_ggml_tensor * flash_grad = NULL;
                if (src0->grad || src1->grad || tensor->src[2]->grad) {
                    int32_t t = lm_ggml_get_op_params_i32(tensor, 0);
                    LM_GGML_ASSERT(t == 0 || t == 1);
                    bool masked = t != 0;
                    flash_grad =
                        lm_ggml_flash_attn_back(ctx,
                            src0,
                            src1,
                            tensor->src[2],
                            tensor->grad,
                            masked);
                }

                struct lm_ggml_tensor * src2 = tensor->src[2];
                const int64_t elem_q = lm_ggml_nelements(src0);
                const int64_t elem_k = lm_ggml_nelements(src1);
                const int64_t elem_v = lm_ggml_nelements(src2);

                enum lm_ggml_type result_type = flash_grad->type;
                LM_GGML_ASSERT(lm_ggml_blck_size(result_type) == 1);
                const size_t tsize = lm_ggml_type_size(result_type);

                const size_t offs_q = 0;
                const size_t offs_k = offs_q + LM_GGML_PAD(elem_q * tsize, LM_GGML_MEM_ALIGN);
                const size_t offs_v = offs_k + LM_GGML_PAD(elem_k * tsize, LM_GGML_MEM_ALIGN);

                if (src0->grad) {
                    struct lm_ggml_tensor * view_q = lm_ggml_view_1d(ctx, flash_grad, elem_q, offs_q);
                    struct lm_ggml_tensor * grad_q = lm_ggml_reshape(ctx, view_q, src0);
                    src0->grad = lm_ggml_add_or_set(ctx,
                            src0->grad,
                            grad_q,
                            zero_table);
                }
                if (src1->grad) {
                    struct lm_ggml_tensor * view_k = lm_ggml_view_1d(ctx, flash_grad, elem_k, offs_k);
                    struct lm_ggml_tensor * grad_k = lm_ggml_reshape(ctx, view_k, src1);
                    src1->grad = lm_ggml_add_or_set(ctx,
                            src1->grad,
                            grad_k,
                            zero_table);
                }
                if (src2->grad) {
                    struct lm_ggml_tensor * view_v = lm_ggml_view_1d(ctx, flash_grad, elem_v, offs_v);
                    struct lm_ggml_tensor * grad_v = lm_ggml_reshape(ctx, view_v, src2);
                    src2->grad = lm_ggml_add_or_set(ctx,
                            src2->grad,
                            grad_v,
                            zero_table);
                }
            } break;
        case LM_GGML_OP_FLASH_FF:
            {
                LM_GGML_ASSERT(false); // not supported
            } break;
        case LM_GGML_OP_FLASH_ATTN_BACK:
            {
                LM_GGML_ASSERT(false); // not supported
            } break;
        case LM_GGML_OP_WIN_PART:
        case LM_GGML_OP_WIN_UNPART:
        case LM_GGML_OP_UNARY:
            {
                switch (lm_ggml_get_unary_op(tensor)) {
                    case LM_GGML_UNARY_OP_ABS:
                        {
                            if (src0->grad) {
                                src0->grad =
                                    lm_ggml_add_or_set(ctx,
                                            src0->grad,
                                            lm_ggml_mul(ctx,
                                                lm_ggml_sgn(ctx, src0),
                                                tensor->grad),
                                            zero_table);
                            }
                        } break;
                    case LM_GGML_UNARY_OP_SGN:
                        {
                            if (src0->grad) {
                                // noop
                            }
                        } break;
                    case LM_GGML_UNARY_OP_NEG:
                        {
                            if (src0->grad) {
                                src0->grad = lm_ggml_sub_or_set(ctx, src0->grad, tensor->grad, zero_table);
                            }
                        } break;
                    case LM_GGML_UNARY_OP_STEP:
                        {
                            if (src0->grad) {
                                // noop
                            }
                        } break;
                    case LM_GGML_UNARY_OP_TANH:
                        {
                            LM_GGML_ASSERT(false); // TODO: not implemented
                        } break;
                    case LM_GGML_UNARY_OP_ELU:
                        {
                            LM_GGML_ASSERT(false); // TODO: not implemented
                        } break;
                    case LM_GGML_UNARY_OP_RELU:
                        {
                            if (src0->grad) {
                                src0->grad = lm_ggml_add_or_set(ctx,
                                        src0->grad,
                                        lm_ggml_mul(ctx,
                                            lm_ggml_step(ctx, src0),
                                            tensor->grad),
                                        zero_table);
                            }
                        } break;
                    case LM_GGML_UNARY_OP_GELU:
                        {
                            LM_GGML_ASSERT(false); // TODO: not implemented
                        } break;
                    case LM_GGML_UNARY_OP_GELU_QUICK:
                        {
                            LM_GGML_ASSERT(false); // TODO: not implemented
                        } break;
                    case LM_GGML_UNARY_OP_SILU:
                        {
                            // necessary for llama
                            if (src0->grad) {
                                src0->grad = lm_ggml_add_or_set(ctx,
                                        src0->grad,
                                        lm_ggml_silu_back(ctx, src0, tensor->grad),
                                        zero_table);
                            }
                        } break;
                    default:
                        LM_GGML_ASSERT(false);
                }
            } break;
        case LM_GGML_OP_GET_REL_POS:
        case LM_GGML_OP_ADD_REL_POS:
        case LM_GGML_OP_MAP_UNARY:
        case LM_GGML_OP_MAP_BINARY:
        case LM_GGML_OP_MAP_CUSTOM1_F32:
        case LM_GGML_OP_MAP_CUSTOM2_F32:
        case LM_GGML_OP_MAP_CUSTOM3_F32:
        case LM_GGML_OP_MAP_CUSTOM1:
        case LM_GGML_OP_MAP_CUSTOM2:
        case LM_GGML_OP_MAP_CUSTOM3:
            {
                LM_GGML_ASSERT(false); // not supported
            } break;
        case LM_GGML_OP_CROSS_ENTROPY_LOSS:
            {
                if (src0->grad) {
                    src0->grad = lm_ggml_add_or_set(ctx,
                                src0->grad,
                                lm_ggml_cross_entropy_loss_back(ctx,
                                    src0,
                                    src1,
                                    tensor->grad),
                                zero_table);
                }
            } break;
        case LM_GGML_OP_CROSS_ENTROPY_LOSS_BACK:
            {
                LM_GGML_ASSERT(false); // not supported
            } break;
        case LM_GGML_OP_NONE:
            {
                // nop
            } break;
        case LM_GGML_OP_COUNT:
            {
                LM_GGML_ASSERT(false);
            } break;
    }

    for (int i = 0; i < LM_GGML_MAX_SRC; ++i) {
        if (tensor->src[i] && tensor->src[i]->grad) {
            LM_GGML_ASSERT(lm_ggml_are_same_shape(tensor->src[i], tensor->src[i]->grad));
        }
    }
}

static void lm_ggml_visit_parents(struct lm_ggml_cgraph * cgraph, struct lm_ggml_tensor * node) {
    if (node->grad == NULL) {
        // this usually happens when we generate intermediate nodes from constants in the backward pass
        // it can also happen during forward pass, if the user performs computations with constants
        if (node->op != LM_GGML_OP_NONE) {
            //LM_GGML_PRINT_DEBUG("%s: warning: node %p has no grad, but op %d\n", __func__, (void *) node, node->op);
        }
    }

    // check if already visited
    if (hash_insert(cgraph->visited_hash_table, node)) {
        return;
    }

    for (int i = 0; i < LM_GGML_MAX_SRC; ++i) {
        const int k =
            (cgraph->order == LM_GGML_CGRAPH_EVAL_ORDER_LEFT_TO_RIGHT) ? i :
            (cgraph->order == LM_GGML_CGRAPH_EVAL_ORDER_RIGHT_TO_LEFT) ? (LM_GGML_MAX_SRC-1-i) :
            /* unknown order, just fall back to using i*/ i;
        if (node->src[k]) {
            lm_ggml_visit_parents(cgraph, node->src[k]);
        }
    }

    if (node->op == LM_GGML_OP_NONE && node->grad == NULL) {
        // reached a leaf node, not part of the gradient graph (e.g. a constant)
        LM_GGML_ASSERT(cgraph->n_leafs < LM_GGML_MAX_NODES);

        if (strlen(node->name) == 0) {
            lm_ggml_format_name(node, "leaf_%d", cgraph->n_leafs);
        }

        cgraph->leafs[cgraph->n_leafs] = node;
        cgraph->n_leafs++;
    } else {
        LM_GGML_ASSERT(cgraph->n_nodes < LM_GGML_MAX_NODES);

        if (strlen(node->name) == 0) {
            lm_ggml_format_name(node, "node_%d", cgraph->n_nodes);
        }

        cgraph->nodes[cgraph->n_nodes] = node;
        cgraph->grads[cgraph->n_nodes] = node->grad;
        cgraph->n_nodes++;
    }
}

static void lm_ggml_build_forward_impl(struct lm_ggml_cgraph * cgraph, struct lm_ggml_tensor * tensor, bool expand) {
    if (!expand) {
        cgraph->n_nodes = 0;
        cgraph->n_leafs = 0;
    }

    const int n0 = cgraph->n_nodes;
    UNUSED(n0);

    lm_ggml_visit_parents(cgraph, tensor);

    const int n_new = cgraph->n_nodes - n0;
    LM_GGML_PRINT_DEBUG("%s: visited %d new nodes\n", __func__, n_new);

    if (n_new > 0) {
        // the last added node should always be starting point
        LM_GGML_ASSERT(cgraph->nodes[cgraph->n_nodes - 1] == tensor);
    }
}

void lm_ggml_build_forward_expand(struct lm_ggml_cgraph * cgraph, struct lm_ggml_tensor * tensor) {
    lm_ggml_build_forward_impl(cgraph, tensor, true);
}

struct lm_ggml_cgraph lm_ggml_build_forward(struct lm_ggml_tensor * tensor) {
    struct lm_ggml_cgraph result = {
        /*.n_nodes      =*/ 0,
        /*.n_leafs      =*/ 0,
        /*.nodes        =*/ { NULL },
        /*.grads        =*/ { NULL },
        /*.leafs        =*/ { NULL },
        /*.hash_table   =*/ { NULL },
        /*.order        =*/ LM_GGML_CGRAPH_EVAL_ORDER_LEFT_TO_RIGHT,
        /*.perf_runs    =*/ 0,
        /*.perf_cycles  =*/ 0,
        /*.perf_time_us =*/ 0,
    };

    lm_ggml_build_forward_impl(&result, tensor, false);

    return result;
}

void lm_ggml_build_backward_expand(struct lm_ggml_context * ctx, struct lm_ggml_cgraph * gf, struct lm_ggml_cgraph * gb, bool keep) {
    LM_GGML_ASSERT(gf->n_nodes > 0);

    // if we are keeping the gradient graph, we have to detach the gradient nodes from the original graph
    if (keep) {
        for (int i = 0; i < gf->n_nodes; i++) {
            struct lm_ggml_tensor * node = gf->nodes[i];

            if (node->grad) {
                node->grad = lm_ggml_dup_tensor(ctx, node);
                gf->grads[i] = node->grad;
            }
        }
    }

    // remember original gradients which start with zero values
    void ** zero_table = malloc(sizeof(void *) * LM_GGML_GRAPH_HASHTABLE_SIZE);
    memset(zero_table, 0, sizeof(void*) * LM_GGML_GRAPH_HASHTABLE_SIZE);
    for (int i = 0; i < gf->n_nodes; i++) {
        if (gf->grads[i]) {
            hash_insert(zero_table, gf->grads[i]);
        }
    }

    for (int i = gf->n_nodes - 1; i >= 0; i--) {
        struct lm_ggml_tensor * node = gf->nodes[i];

        // inplace operations to add gradients are not created by lm_ggml_compute_backward
        // use allocator to automatically make inplace operations
        if (node->grad) {
            lm_ggml_compute_backward(ctx, node, zero_table);
        }
    }

    for (int i = 0; i < gf->n_nodes; i++) {
        struct lm_ggml_tensor * node = gf->nodes[i];

        if (node->is_param) {
            LM_GGML_PRINT_DEBUG("%s: found root node %p\n", __func__, (void *) node);
            lm_ggml_build_forward_expand(gb, node->grad);
        }
    }

    free(zero_table);
}

struct lm_ggml_cgraph lm_ggml_build_backward(struct lm_ggml_context * ctx, struct lm_ggml_cgraph * gf, bool keep) {
    struct lm_ggml_cgraph result = *gf;
    lm_ggml_build_backward_expand(ctx, gf, &result, keep);
    return result;
}

struct lm_ggml_cgraph * lm_ggml_new_graph(struct lm_ggml_context * ctx) {
    struct lm_ggml_object * obj = lm_ggml_new_object(ctx, LM_GGML_OBJECT_GRAPH, LM_GGML_GRAPH_SIZE);
    struct lm_ggml_cgraph * cgraph = (struct lm_ggml_cgraph *) ((char *) ctx->mem_buffer + obj->offs);

    *cgraph = (struct lm_ggml_cgraph) {
        /*.n_nodes      =*/ 0,
        /*.n_leafs      =*/ 0,
        /*.nodes        =*/ { NULL },
        /*.grads        =*/ { NULL },
        /*.leafs        =*/ { NULL },
        /*.hash_table   =*/ { NULL },
        /*.order        =*/ LM_GGML_CGRAPH_EVAL_ORDER_LEFT_TO_RIGHT,
        /*.perf_runs    =*/ 0,
        /*.perf_cycles  =*/ 0,
        /*.perf_time_us =*/ 0,
    };

    return cgraph;
}

struct lm_ggml_cgraph * lm_ggml_build_forward_ctx(struct lm_ggml_context * ctx, struct lm_ggml_tensor * tensor) {
    struct lm_ggml_cgraph * cgraph = lm_ggml_new_graph(ctx);
    lm_ggml_build_forward_impl(cgraph, tensor, false);
    return cgraph;
}

size_t lm_ggml_graph_overhead(void) {
    return LM_GGML_OBJECT_SIZE + LM_GGML_PAD(LM_GGML_GRAPH_SIZE, LM_GGML_MEM_ALIGN);
}

//
// thread data
//
// synchronization is done via busy loops
// I tried using spin locks, but not sure how to use them correctly - the things I tried were slower than busy loops
//

#ifdef __APPLE__

//#include <os/lock.h>
//
//typedef os_unfair_lock lm_ggml_lock_t;
//
//#define lm_ggml_lock_init(x)    UNUSED(x)
//#define lm_ggml_lock_destroy(x) UNUSED(x)
//#define lm_ggml_lock_lock       os_unfair_lock_lock
//#define lm_ggml_lock_unlock     os_unfair_lock_unlock
//
//#define LM_GGML_LOCK_INITIALIZER OS_UNFAIR_LOCK_INIT

typedef int lm_ggml_lock_t;

#define lm_ggml_lock_init(x)    UNUSED(x)
#define lm_ggml_lock_destroy(x) UNUSED(x)
#define lm_ggml_lock_lock(x)    UNUSED(x)
#define lm_ggml_lock_unlock(x)  UNUSED(x)

#define LM_GGML_LOCK_INITIALIZER 0

typedef pthread_t lm_ggml_thread_t;

#define lm_ggml_thread_create pthread_create
#define lm_ggml_thread_join   pthread_join

#else

//typedef pthread_spinlock_t lm_ggml_lock_t;

//#define lm_ggml_lock_init(x) pthread_spin_init(x, PTHREAD_PROCESS_PRIVATE)
//#define lm_ggml_lock_destroy pthread_spin_destroy
//#define lm_ggml_lock_lock    pthread_spin_lock
//#define lm_ggml_lock_unlock  pthread_spin_unlock

typedef int lm_ggml_lock_t;

#define lm_ggml_lock_init(x)    UNUSED(x)
#define lm_ggml_lock_destroy(x) UNUSED(x)
#if defined(__x86_64__) || (defined(_MSC_VER) && defined(_M_AMD64))
#define lm_ggml_lock_lock(x)    _mm_pause()
#else
#define lm_ggml_lock_lock(x)    UNUSED(x)
#endif
#define lm_ggml_lock_unlock(x)  UNUSED(x)

#define LM_GGML_LOCK_INITIALIZER 0

typedef pthread_t lm_ggml_thread_t;

#define lm_ggml_thread_create pthread_create
#define lm_ggml_thread_join   pthread_join

#endif

// Android's libc implementation "bionic" does not support setting affinity
#if defined(__linux__) && !defined(__BIONIC__)
static void set_numa_thread_affinity(int thread_n, int n_threads) {
    if (!lm_ggml_is_numa()) {
        return;
    }

    // run thread on node_num thread_n / (threads per node)
    const int node_num = thread_n / ((n_threads + g_state.numa.n_nodes - 1) / g_state.numa.n_nodes);
    struct lm_ggml_numa_node * node = &g_state.numa.nodes[node_num];
    size_t setsize = CPU_ALLOC_SIZE(g_state.numa.total_cpus);

    cpu_set_t * cpus = CPU_ALLOC(g_state.numa.total_cpus);
    CPU_ZERO_S(setsize, cpus);
    for (size_t i = 0; i < node->n_cpus; ++i) {
        CPU_SET_S(node->cpus[i], setsize, cpus);
    }

    int rv = pthread_setaffinity_np(pthread_self(), setsize, cpus);
    if (rv) {
            fprintf(stderr, "warning: pthread_setaffinity_np() failed: %s\n",
                    strerror(rv));
    }

    CPU_FREE(cpus);
}

static void clear_numa_thread_affinity(void) {
    if (!lm_ggml_is_numa()) {
        return;
    }

    size_t setsize = CPU_ALLOC_SIZE(g_state.numa.total_cpus);

    cpu_set_t * cpus = CPU_ALLOC(g_state.numa.total_cpus);
    CPU_ZERO_S(setsize, cpus);
    for (unsigned i = 0; i < g_state.numa.total_cpus; ++i) {
        CPU_SET_S(i, setsize, cpus);
    }

    int rv = pthread_setaffinity_np(pthread_self(), setsize, cpus);
    if (rv) {
        fprintf(stderr, "warning: pthread_setaffinity_np() failed: %s\n",
            strerror(rv));
    }

    CPU_FREE(cpus);
}
#else
// TODO: Windows etc.
// (the linux implementation may also work on BSD, someone should test)
static void set_numa_thread_affinity(int thread_n, int n_threads) { UNUSED(thread_n); UNUSED(n_threads);  }
static void clear_numa_thread_affinity(void) {}
#endif

struct lm_ggml_compute_state_shared {
    const struct lm_ggml_cgraph * cgraph;
    const struct lm_ggml_cplan  * cplan;

    int64_t perf_node_start_cycles;
    int64_t perf_node_start_time_us;

    const int n_threads;

    // synchronization primitives
    atomic_int n_active; // num active threads
    atomic_int node_n;   // active graph node

    bool (*abort_callback)(void * data); // abort lm_ggml_graph_compute when true
    void * abort_callback_data;
};

struct lm_ggml_compute_state {
    lm_ggml_thread_t thrd;
    int ith;
    struct lm_ggml_compute_state_shared * shared;
};

static void lm_ggml_graph_compute_perf_stats_node(struct lm_ggml_tensor * node, const struct lm_ggml_compute_state_shared * st) {
    int64_t cycles_cur  = lm_ggml_perf_cycles()  - st->perf_node_start_cycles;
    int64_t time_us_cur = lm_ggml_perf_time_us() - st->perf_node_start_time_us;

    node->perf_runs++;
    node->perf_cycles  += cycles_cur;
    node->perf_time_us += time_us_cur;
}

static thread_ret_t lm_ggml_graph_compute_thread(void * data) {
    struct lm_ggml_compute_state * state = (struct lm_ggml_compute_state *) data;

    const struct lm_ggml_cgraph * cgraph = state->shared->cgraph;
    const struct lm_ggml_cplan  * cplan  = state->shared->cplan;

    const int * n_tasks_arr = cplan->n_tasks;
    const int   n_threads   = state->shared->n_threads;

    set_numa_thread_affinity(state->ith, n_threads);

    int node_n = -1;

    while (true) {
        if (cplan->abort_callback && cplan->abort_callback(cplan->abort_callback_data)) {
            state->shared->node_n += 1;
            return (thread_ret_t) LM_GGML_EXIT_ABORTED;
        }
        if (atomic_fetch_sub(&state->shared->n_active, 1) == 1) {
            // all other threads are finished and spinning
            // do finalize and init here so we don't have synchronize again
            struct lm_ggml_compute_params params = {
                /*.type  =*/ LM_GGML_TASK_FINALIZE,
                /*.ith   =*/ 0,
                /*.nth   =*/ 0,
                /*.wsize =*/ cplan->work_size,
                /*.wdata =*/ cplan->work_data,
            };

            if (node_n != -1) {
                /* FINALIZE */
                struct lm_ggml_tensor * node = state->shared->cgraph->nodes[node_n];
                if (LM_GGML_OP_HAS_FINALIZE[node->op]) {
                    params.nth = n_tasks_arr[node_n];
                    lm_ggml_compute_forward(&params, node);
                }
                lm_ggml_graph_compute_perf_stats_node(node, state->shared);
            }

            // distribute new work or execute it direct if 1T
            while (++node_n < cgraph->n_nodes) {
                LM_GGML_PRINT_DEBUG_5("%s: %d/%d\n", __func__, node_n, cgraph->n_nodes);

                struct lm_ggml_tensor * node = cgraph->nodes[node_n];
                const int n_tasks = n_tasks_arr[node_n];

                state->shared->perf_node_start_cycles  = lm_ggml_perf_cycles();
                state->shared->perf_node_start_time_us = lm_ggml_perf_time_us();

                params.nth = n_tasks;

                /* INIT */
                if (LM_GGML_OP_HAS_INIT[node->op]) {
                    params.type = LM_GGML_TASK_INIT;
                    lm_ggml_compute_forward(&params, node);
                }

                if (n_tasks == 1) {
                    // TODO: maybe push node_n to the atomic but if other threads see n_tasks is 1,
                    // they do something more efficient than spinning (?)
                    params.type = LM_GGML_TASK_COMPUTE;
                    lm_ggml_compute_forward(&params, node);

                    if (LM_GGML_OP_HAS_FINALIZE[node->op]) {
                        params.type = LM_GGML_TASK_FINALIZE;
                        lm_ggml_compute_forward(&params, node);
                    }

                    lm_ggml_graph_compute_perf_stats_node(node, state->shared);
                } else {
                    break;
                }

                if (cplan->abort_callback && cplan->abort_callback(cplan->abort_callback_data)) {
                    break;
                }
            }

            atomic_store(&state->shared->n_active, n_threads);
            atomic_store(&state->shared->node_n,   node_n);
        } else {
            // wait for other threads to finish
            const int last = node_n;
            while (true) {
                // TODO: this sched_yield can have significant impact on the performance - either positive or negative
                //       depending on the workload and the operating system.
                //       since it is not clear what is the best approach, it should potentially become user-configurable
                //       ref: https://github.com/ggerganov/ggml/issues/291
#if defined(LM_GGML_USE_ACCELERATE) || defined(LM_GGML_USE_OPENBLAS)
                sched_yield();
#endif

                node_n = atomic_load(&state->shared->node_n);
                if (node_n != last) break;
            };
        }

        // check if we should stop
        if (node_n >= cgraph->n_nodes) break;

        /* COMPUTE */
        struct lm_ggml_tensor * node = cgraph->nodes[node_n];
        const int n_tasks = n_tasks_arr[node_n];

        struct lm_ggml_compute_params params = {
            /*.type  =*/ LM_GGML_TASK_COMPUTE,
            /*.ith   =*/ state->ith,
            /*.nth   =*/ n_tasks,
            /*.wsize =*/ cplan->work_size,
            /*.wdata =*/ cplan->work_data,
        };

        if (state->ith < n_tasks) {
            lm_ggml_compute_forward(&params, node);
        }
    }

    return LM_GGML_EXIT_SUCCESS;
}

struct lm_ggml_cplan lm_ggml_graph_plan(struct lm_ggml_cgraph * cgraph, int n_threads) {
    if (n_threads <= 0) {
        n_threads = LM_GGML_DEFAULT_N_THREADS;
    }

    size_t work_size = 0;

    struct lm_ggml_cplan cplan;
    memset(&cplan, 0, sizeof(struct lm_ggml_cplan));

    // thread scheduling for the different operations + work buffer size estimation
    for (int i = 0; i < cgraph->n_nodes; i++) {
        int n_tasks = 1;

        struct lm_ggml_tensor * node = cgraph->nodes[i];

        switch (node->op) {
            case LM_GGML_OP_CPY:
            case LM_GGML_OP_DUP:
                {
                    n_tasks = n_threads;

                    size_t cur = 0;
                    if (lm_ggml_is_quantized(node->type)) {
                        cur = lm_ggml_type_size(LM_GGML_TYPE_F32) * node->ne[0] * n_tasks;
                    }

                    work_size = MAX(work_size, cur);
                } break;
            case LM_GGML_OP_ADD:
            case LM_GGML_OP_ADD1:
                {
                    n_tasks = n_threads;

                    size_t cur = 0;

                    if (lm_ggml_is_quantized(node->src[0]->type)) {
                        cur = lm_ggml_type_size(LM_GGML_TYPE_F32) * node->src[0]->ne[0] * n_tasks;
                    }

                    work_size = MAX(work_size, cur);
                } break;
            case LM_GGML_OP_ACC:
                {
                    n_tasks = n_threads;

                    size_t cur = 0;

                    if (lm_ggml_is_quantized(node->src[0]->type)) {
                        cur = lm_ggml_type_size(LM_GGML_TYPE_F32) * node->src[1]->ne[0] * n_tasks;
                    }

                    work_size = MAX(work_size, cur);
                } break;
            case LM_GGML_OP_SUB:
            case LM_GGML_OP_DIV:
            case LM_GGML_OP_SQR:
            case LM_GGML_OP_SQRT:
            case LM_GGML_OP_LOG:
            case LM_GGML_OP_SUM:
            case LM_GGML_OP_SUM_ROWS:
            case LM_GGML_OP_MEAN:
            case LM_GGML_OP_ARGMAX:
            case LM_GGML_OP_REPEAT:
            case LM_GGML_OP_REPEAT_BACK:
            {
                    n_tasks = 1;
                } break;

            case LM_GGML_OP_UNARY:
                {
                    switch (lm_ggml_get_unary_op(node)) {
                        case LM_GGML_UNARY_OP_ABS:
                        case LM_GGML_UNARY_OP_SGN:
                        case LM_GGML_UNARY_OP_NEG:
                        case LM_GGML_UNARY_OP_STEP:
                        case LM_GGML_UNARY_OP_TANH:
                        case LM_GGML_UNARY_OP_ELU:
                        case LM_GGML_UNARY_OP_RELU:
                            {
                                n_tasks = 1;
                            } break;

                        case LM_GGML_UNARY_OP_GELU:
                        case LM_GGML_UNARY_OP_GELU_QUICK:
                        case LM_GGML_UNARY_OP_SILU:
                            {
                                n_tasks = n_threads;
                            } break;
                    }
                } break;
            case LM_GGML_OP_SILU_BACK:
            case LM_GGML_OP_MUL:
            case LM_GGML_OP_NORM:
            case LM_GGML_OP_RMS_NORM:
            case LM_GGML_OP_RMS_NORM_BACK:
            case LM_GGML_OP_GROUP_NORM:
                {
                    n_tasks = n_threads;
                } break;
            case LM_GGML_OP_CONCAT:
            case LM_GGML_OP_MUL_MAT:
                {
                    n_tasks = n_threads;

                    // TODO: use different scheduling for different matrix sizes
                    //const int nr0 = lm_ggml_nrows(node->src[0]);
                    //const int nr1 = lm_ggml_nrows(node->src[1]);

                    //n_tasks = MIN(n_threads, MAX(1, nr0/128));
                    //printf("nr0 = %8d, nr1 = %8d, nr0*nr1 = %8d, n_tasks%d\n", nr0, nr1, nr0*nr1, n_tasks);

                    size_t cur = 0;
                    const enum lm_ggml_type vec_dot_type = type_traits[node->src[0]->type].vec_dot_type;

#if defined(LM_GGML_USE_CUBLAS)
                    if (lm_ggml_cuda_can_mul_mat(node->src[0], node->src[1], node)) {
                        n_tasks = 1; // TODO: this actually is doing nothing
                                     //       the threads are still spinning
                    } else
#elif defined(LM_GGML_USE_CLBLAST)
                    if (lm_ggml_cl_can_mul_mat(node->src[0], node->src[1], node)) {
                        n_tasks = 1; // TODO: this actually is doing nothing
                                     //       the threads are still spinning
                        cur = lm_ggml_cl_mul_mat_get_wsize(node->src[0], node->src[1], node);
                    } else
#endif
#if defined(LM_GGML_USE_ACCELERATE) || defined(LM_GGML_USE_OPENBLAS)
                    if (lm_ggml_compute_forward_mul_mat_use_blas(node->src[0], node->src[1], node)) {
                        n_tasks = 1; // TODO: this actually is doing nothing
                                     //       the threads are still spinning
                        if (node->src[0]->type != LM_GGML_TYPE_F32) {
                            // here we need memory just for single 2D matrix from src0
                            cur = lm_ggml_type_size(LM_GGML_TYPE_F32)*(node->src[0]->ne[0]*node->src[0]->ne[1]);
                        }
                    } else
#endif
                    if (node->src[1]->type != vec_dot_type) {
                        cur = lm_ggml_type_size(vec_dot_type)*lm_ggml_nelements(node->src[1])/lm_ggml_blck_size(vec_dot_type);
                    } else {
                        cur = 0;
                    }

                    work_size = MAX(work_size, cur);
                } break;
            case LM_GGML_OP_OUT_PROD:
                {
                    n_tasks = n_threads;

                    size_t cur = 0;

                    if (lm_ggml_is_quantized(node->src[0]->type)) {
                        cur = lm_ggml_type_size(LM_GGML_TYPE_F32) * node->src[0]->ne[0] * n_tasks;
                    }

                    work_size = MAX(work_size, cur);
                } break;
            case LM_GGML_OP_SCALE:
                {
                    n_tasks = 1;
                } break;
            case LM_GGML_OP_SET:
            case LM_GGML_OP_CONT:
            case LM_GGML_OP_RESHAPE:
            case LM_GGML_OP_VIEW:
            case LM_GGML_OP_PERMUTE:
            case LM_GGML_OP_TRANSPOSE:
            case LM_GGML_OP_GET_ROWS:
            case LM_GGML_OP_GET_ROWS_BACK:
            case LM_GGML_OP_DIAG:
                {
                    n_tasks = 1;
                } break;
            case LM_GGML_OP_DIAG_MASK_ZERO:
            case LM_GGML_OP_DIAG_MASK_INF:
            case LM_GGML_OP_SOFT_MAX:
            case LM_GGML_OP_SOFT_MAX_BACK:
            case LM_GGML_OP_ROPE:
            case LM_GGML_OP_ROPE_BACK:
            case LM_GGML_OP_ADD_REL_POS:
                {
                    n_tasks = n_threads;
                } break;
            case LM_GGML_OP_ALIBI:
                {
                    n_tasks = 1; //TODO
                } break;
            case LM_GGML_OP_CLAMP:
                {
                    n_tasks = 1; //TODO
                } break;
            case LM_GGML_OP_CONV_1D:
                {
                    n_tasks = n_threads;

                    LM_GGML_ASSERT(node->src[0]->ne[3] == 1);
                    LM_GGML_ASSERT(node->src[1]->ne[2] == 1);
                    LM_GGML_ASSERT(node->src[1]->ne[3] == 1);

                    const int64_t ne00 = node->src[0]->ne[0];
                    const int64_t ne01 = node->src[0]->ne[1];
                    const int64_t ne02 = node->src[0]->ne[2];

                    const int64_t ne10 = node->src[1]->ne[0];
                    const int64_t ne11 = node->src[1]->ne[1];

                    const int64_t ne0 = node->ne[0];
                    const int64_t ne1 = node->ne[1];
                    const int64_t nk  = ne00;
                    const int64_t ew0 = nk * ne01;

                    UNUSED(ne02);
                    UNUSED(ne10);
                    UNUSED(ne11);

                    size_t cur = 0;

                    if (node->src[0]->type == LM_GGML_TYPE_F16 &&
                        node->src[1]->type == LM_GGML_TYPE_F32) {
                        cur = sizeof(lm_ggml_fp16_t)*(ne0*ne1*ew0);
                    } else if (node->src[0]->type == LM_GGML_TYPE_F32 &&
                               node->src[1]->type == LM_GGML_TYPE_F32) {
                        cur = sizeof(float)*(ne0*ne1*ew0);
                    } else {
                        LM_GGML_ASSERT(false);
                    }

                    work_size = MAX(work_size, cur);
                } break;
            case LM_GGML_OP_CONV_1D_STAGE_0:
                {
                    n_tasks = n_threads;
                } break;
            case LM_GGML_OP_CONV_1D_STAGE_1:
                {
                    n_tasks = n_threads;
                } break;
            case LM_GGML_OP_CONV_TRANSPOSE_1D:
                {
                    n_tasks = n_threads;

                    LM_GGML_ASSERT(node->src[0]->ne[3] == 1);
                    LM_GGML_ASSERT(node->src[1]->ne[2] == 1);
                    LM_GGML_ASSERT(node->src[1]->ne[3] == 1);

                    const int64_t ne00 = node->src[0]->ne[0];  // K
                    const int64_t ne01 = node->src[0]->ne[1];  // Cout
                    const int64_t ne02 = node->src[0]->ne[2];  // Cin

                    const int64_t ne10 = node->src[1]->ne[0];  // L
                    const int64_t ne11 = node->src[1]->ne[1];  // Cin

                    size_t cur = 0;
                    if (node->src[0]->type == LM_GGML_TYPE_F16 &&
                        node->src[1]->type == LM_GGML_TYPE_F32) {
                        cur += sizeof(lm_ggml_fp16_t)*ne00*ne01*ne02;
                        cur += sizeof(lm_ggml_fp16_t)*ne10*ne11;
                    } else if (node->src[0]->type == LM_GGML_TYPE_F32 &&
                               node->src[1]->type == LM_GGML_TYPE_F32) {
                        cur += sizeof(float)*ne00*ne01*ne02;
                        cur += sizeof(float)*ne10*ne11;
                    } else {
                        LM_GGML_ASSERT(false);
                    }

                    work_size = MAX(work_size, cur);
                } break;
            case LM_GGML_OP_CONV_2D:
                {
                    n_tasks = n_threads;

                    const int64_t ne00 = node->src[0]->ne[0]; // W
                    const int64_t ne01 = node->src[0]->ne[1]; // H
                    const int64_t ne02 = node->src[0]->ne[2]; // C
                    const int64_t ne03 = node->src[0]->ne[3]; // N

                    const int64_t ne10 = node->src[1]->ne[0]; // W
                    const int64_t ne11 = node->src[1]->ne[1]; // H
                    const int64_t ne12 = node->src[1]->ne[2]; // C

                    const int64_t ne0 = node->ne[0];
                    const int64_t ne1 = node->ne[1];
                    const int64_t ne2 = node->ne[2];
                    const int64_t ne3 = node->ne[3];
                    const int64_t nk = ne00*ne01;
                    const int64_t ew0 = nk * ne02;

                    UNUSED(ne03);
                    UNUSED(ne2);

                    size_t cur = 0;

                    if (node->src[0]->type == LM_GGML_TYPE_F16 &&
                        node->src[1]->type == LM_GGML_TYPE_F32) {
                        // im2col: [N*OH*OW, IC*KH*KW]
                        cur = sizeof(lm_ggml_fp16_t)*(ne3*ne0*ne1*ew0);
                    } else if (node->src[0]->type == LM_GGML_TYPE_F32 &&
                               node->src[1]->type == LM_GGML_TYPE_F32) {
                        cur = sizeof(float)*      (ne10*ne11*ne12);
                    } else {
                        LM_GGML_ASSERT(false);
                    }

                    work_size = MAX(work_size, cur);
                } break;
            case LM_GGML_OP_CONV_2D_STAGE_0:
                {
                    n_tasks = n_threads;
                } break;
            case LM_GGML_OP_CONV_2D_STAGE_1:
                {
                    n_tasks = n_threads;
                } break;
            case LM_GGML_OP_CONV_TRANSPOSE_2D:
                {
                    n_tasks = n_threads;

                    const int64_t ne00 = node->src[0]->ne[0]; // W
                    const int64_t ne01 = node->src[0]->ne[1]; // H
                    const int64_t ne02 = node->src[0]->ne[2]; // Channels Out
                    const int64_t ne03 = node->src[0]->ne[3]; // Channels In

                    const int64_t ne10 = node->src[1]->ne[0]; // W
                    const int64_t ne11 = node->src[1]->ne[1]; // H
                    const int64_t ne12 = node->src[1]->ne[2]; // Channels In

                    size_t cur = 0;
                    cur += sizeof(lm_ggml_fp16_t)*ne00*ne01*ne02*ne03;
                    cur += sizeof(lm_ggml_fp16_t)*ne10*ne11*ne12;

                    work_size = MAX(work_size, cur);
                } break;
            case LM_GGML_OP_POOL_1D:
            case LM_GGML_OP_POOL_2D:
                {
                    n_tasks = 1;
                } break;
            case LM_GGML_OP_UPSCALE:
                {
                    n_tasks = n_threads;
                } break;
            case LM_GGML_OP_FLASH_ATTN:
                {
                    n_tasks = n_threads;

                    size_t cur = 0;

                    const int64_t ne11 = lm_ggml_up(node->src[1]->ne[1], LM_GGML_SOFT_MAX_UNROLL);

                    if (node->src[1]->type == LM_GGML_TYPE_F32) {
                        cur  = sizeof(float)*ne11*n_tasks; // TODO: this can become (n_tasks-1)
                        cur += sizeof(float)*ne11*n_tasks; // this is overestimated by x2
                    }

                    if (node->src[1]->type == LM_GGML_TYPE_F16) {
                        cur  = sizeof(float)*ne11*n_tasks; // TODO: this can become (n_tasks-1)
                        cur += sizeof(float)*ne11*n_tasks; // this is overestimated by x2
                    }

                    work_size = MAX(work_size, cur);
                } break;
            case LM_GGML_OP_FLASH_FF:
                {
                    n_tasks = n_threads;

                    size_t cur = 0;

                    if (node->src[1]->type == LM_GGML_TYPE_F32) {
                        cur  = sizeof(float)*node->src[1]->ne[1]*n_tasks; // TODO: this can become (n_tasks-1)
                        cur += sizeof(float)*node->src[1]->ne[1]*n_tasks; // this is overestimated by x2
                    }

                    if (node->src[1]->type == LM_GGML_TYPE_F16) {
                        cur  = sizeof(float)*node->src[1]->ne[1]*n_tasks; // TODO: this can become (n_tasks-1)
                        cur += sizeof(float)*node->src[1]->ne[1]*n_tasks; // this is overestimated by x2
                    }

                    work_size = MAX(work_size, cur);
                } break;
            case LM_GGML_OP_FLASH_ATTN_BACK:
                {
                    n_tasks = n_threads;

                    size_t cur = 0;

                    const int64_t    D = node->src[0]->ne[0];
                    const int64_t ne11 = lm_ggml_up(node->src[1]->ne[1], LM_GGML_SOFT_MAX_UNROLL);
                    const int64_t mxDn = MAX(D, ne11) * 2; // *2 because of S and SM in lm_ggml_compute_forward_flash_attn_back
                    if (node->src[1]->type == LM_GGML_TYPE_F32) {
                        cur  = sizeof(float)*mxDn*n_tasks; // TODO: this can become (n_tasks-1)
                        cur += sizeof(float)*mxDn*n_tasks; // this is overestimated by x2
                    }

                    if (node->src[1]->type == LM_GGML_TYPE_F16) {
                        cur  = sizeof(float)*mxDn*n_tasks; // TODO: this can become (n_tasks-1)
                        cur += sizeof(float)*mxDn*n_tasks; // this is overestimated by x2
                    }

                    work_size = MAX(work_size, cur);
                } break;
            case LM_GGML_OP_WIN_PART:
            case LM_GGML_OP_WIN_UNPART:
            case LM_GGML_OP_GET_REL_POS:
            case LM_GGML_OP_MAP_UNARY:
            case LM_GGML_OP_MAP_BINARY:
            case LM_GGML_OP_MAP_CUSTOM1_F32:
            case LM_GGML_OP_MAP_CUSTOM2_F32:
            case LM_GGML_OP_MAP_CUSTOM3_F32:
                {
                    n_tasks = 1;
                } break;
            case LM_GGML_OP_MAP_CUSTOM1:
                {
                    struct lm_ggml_map_custom1_op_params * p = (struct lm_ggml_map_custom1_op_params *) node->op_params;
                    if (p->n_tasks == LM_GGML_N_TASKS_MAX) {
                        n_tasks = n_threads;
                    } else {
                        n_tasks = MIN(p->n_tasks, n_threads);
                    }
                } break;
            case LM_GGML_OP_MAP_CUSTOM2:
                {
                    struct lm_ggml_map_custom2_op_params * p = (struct lm_ggml_map_custom2_op_params *) node->op_params;
                    if (p->n_tasks == LM_GGML_N_TASKS_MAX) {
                        n_tasks = n_threads;
                    } else {
                        n_tasks = MIN(p->n_tasks, n_threads);
                    }
                } break;
            case LM_GGML_OP_MAP_CUSTOM3:
                {
                    struct lm_ggml_map_custom3_op_params * p = (struct lm_ggml_map_custom3_op_params *) node->op_params;
                    if (p->n_tasks == LM_GGML_N_TASKS_MAX) {
                        n_tasks = n_threads;
                    } else {
                        n_tasks = MIN(p->n_tasks, n_threads);
                    }
                } break;
            case LM_GGML_OP_CROSS_ENTROPY_LOSS:
                {
                    n_tasks = n_threads;

                    size_t cur = lm_ggml_type_size(node->type)*(n_tasks + node->src[0]->ne[0]*n_tasks);

                    work_size = MAX(work_size, cur);
                } break;
            case LM_GGML_OP_CROSS_ENTROPY_LOSS_BACK:
                {
                    n_tasks = n_threads;
                } break;
            case LM_GGML_OP_NONE:
                {
                    n_tasks = 1;
                } break;
            case LM_GGML_OP_COUNT:
                {
                    LM_GGML_ASSERT(false);
                } break;
        }

        cplan.n_tasks[i] = n_tasks;
    }

    if (work_size > 0) {
        work_size += CACHE_LINE_SIZE*(n_threads - 1);
    }

    cplan.n_threads = n_threads;
    cplan.work_size = work_size;
    cplan.work_data = NULL;

    return cplan;
}

int lm_ggml_graph_compute(struct lm_ggml_cgraph * cgraph, struct lm_ggml_cplan * cplan) {
    {
        LM_GGML_ASSERT(cplan);
        LM_GGML_ASSERT(cplan->n_threads > 0);

        if (cplan->work_size > 0) {
            LM_GGML_ASSERT(cplan->work_data);
        }

        for (int i = 0; i < cgraph->n_nodes; ++i) {
            if (cgraph->nodes[i]->op != LM_GGML_OP_NONE) {
                LM_GGML_ASSERT(cplan->n_tasks[i] > 0);
            }
        }
    }

    const int n_threads = cplan->n_threads;

    struct lm_ggml_compute_state_shared state_shared = {
        /*.cgraph                  =*/ cgraph,
        /*.cgraph_plan             =*/ cplan,
        /*.perf_node_start_cycles  =*/ 0,
        /*.perf_node_start_time_us =*/ 0,
        /*.n_threads               =*/ n_threads,
        /*.n_active                =*/ n_threads,
        /*.node_n                  =*/ -1,
        /*.abort_callback          =*/ NULL,
        /*.abort_callback_data     =*/ NULL,
    };
    struct lm_ggml_compute_state * workers = alloca(sizeof(struct lm_ggml_compute_state)*n_threads);

    // create thread pool
    if (n_threads > 1) {
        for (int j = 1; j < n_threads; ++j) {
            workers[j] = (struct lm_ggml_compute_state) {
                .thrd   = 0,
                .ith = j,
                .shared = &state_shared,
            };

            const int rc = lm_ggml_thread_create(&workers[j].thrd, NULL, lm_ggml_graph_compute_thread, &workers[j]);
            LM_GGML_ASSERT(rc == 0);
            UNUSED(rc);
        }
    }

    workers[0].ith = 0;
    workers[0].shared = &state_shared;

    const int64_t perf_start_cycles  = lm_ggml_perf_cycles();
    const int64_t perf_start_time_us = lm_ggml_perf_time_us();

    // this is a work thread too
    int compute_status = (size_t) lm_ggml_graph_compute_thread(&workers[0]);

    // don't leave affinity set on the main thread
    clear_numa_thread_affinity();

    // join or kill thread pool
    if (n_threads > 1) {
        for (int j = 1; j < n_threads; j++) {
            const int rc = lm_ggml_thread_join(workers[j].thrd, NULL);
            LM_GGML_ASSERT(rc == 0);
        }
    }

    // performance stats (graph)
    {
        int64_t perf_cycles_cur  = lm_ggml_perf_cycles()  - perf_start_cycles;
        int64_t perf_time_us_cur = lm_ggml_perf_time_us() - perf_start_time_us;

        cgraph->perf_runs++;
        cgraph->perf_cycles  += perf_cycles_cur;
        cgraph->perf_time_us += perf_time_us_cur;

        LM_GGML_PRINT_DEBUG("%s: perf (%d) - cpu = %.3f / %.3f ms, wall = %.3f / %.3f ms\n",
                __func__, cgraph->perf_runs,
                (double) perf_cycles_cur      / (double) lm_ggml_cycles_per_ms(),
                (double) cgraph->perf_cycles  / (double) lm_ggml_cycles_per_ms() / (double) cgraph->perf_runs,
                (double) perf_time_us_cur     / 1000.0,
                (double) cgraph->perf_time_us / 1000.0 / cgraph->perf_runs);
    }

    return compute_status;
}

void lm_ggml_graph_reset(struct lm_ggml_cgraph * cgraph) {
    for (int i = 0; i < cgraph->n_nodes; i++) {
        struct lm_ggml_tensor * grad = cgraph->grads[i];

        if (grad) {
            lm_ggml_set_zero(grad);
        }
    }
}

void lm_ggml_graph_compute_with_ctx(struct lm_ggml_context * ctx, struct lm_ggml_cgraph * cgraph, int n_threads) {
    struct lm_ggml_cplan cplan = lm_ggml_graph_plan(cgraph, n_threads);

    struct lm_ggml_object * obj = lm_ggml_new_object(ctx, LM_GGML_OBJECT_WORK_BUFFER, cplan.work_size);

    cplan.work_data = (uint8_t *)ctx->mem_buffer + obj->offs;

    lm_ggml_graph_compute(cgraph, &cplan);
}

struct lm_ggml_tensor * lm_ggml_graph_get_tensor(struct lm_ggml_cgraph * cgraph, const char * name) {
    for (int i = 0; i < cgraph->n_leafs; i++) {
        struct lm_ggml_tensor * leaf = cgraph->leafs[i];

        if (strcmp(leaf->name, name) == 0) {
            return leaf;
        }
    }

    for (int i = 0; i < cgraph->n_nodes; i++) {
        struct lm_ggml_tensor * node = cgraph->nodes[i];

        if (strcmp(node->name, name) == 0) {
            return node;
        }
    }

    return NULL;
}

static void lm_ggml_graph_export_leaf(const struct lm_ggml_tensor * tensor, FILE * fout) {
    const int64_t * ne = tensor->ne;
    const size_t  * nb = tensor->nb;

    fprintf(fout, "%-6s %-12s %8d %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %16zu %16zu %16zu %16zu %16p %32s\n",
            lm_ggml_type_name(tensor->type),
            lm_ggml_op_name  (tensor->op),
            tensor->n_dims,
            ne[0], ne[1], ne[2], ne[3],
            nb[0], nb[1], nb[2], nb[3],
            tensor->data,
            tensor->name);
}

static void lm_ggml_graph_export_node(const struct lm_ggml_tensor * tensor, const char * arg, FILE * fout) {
    const int64_t * ne = tensor->ne;
    const size_t  * nb = tensor->nb;

    fprintf(fout, "%-6s %-6s %-12s %8d %" PRId64 " %" PRId64 " %" PRId64 " %" PRId64 " %16zu %16zu %16zu %16zu %16p %32s\n",
            arg,
            lm_ggml_type_name(tensor->type),
            lm_ggml_op_name  (tensor->op),
            tensor->n_dims,
            ne[0], ne[1], ne[2], ne[3],
            nb[0], nb[1], nb[2], nb[3],
            tensor->data,
            tensor->name);
}

void lm_ggml_graph_export(const struct lm_ggml_cgraph * cgraph, const char * fname) {
    uint64_t size_eval = 0;

    // compute size of intermediate results
    // TODO: does not take into account scratch buffers !!!!
    for (int i = 0; i < cgraph->n_nodes; ++i) {
        size_eval += lm_ggml_nbytes_pad(cgraph->nodes[i]);
    }

    // print
    {
        FILE * fout = stdout;

        fprintf(fout, "\n");
        fprintf(fout, "%-16s %8x\n", "magic",        LM_GGML_FILE_MAGIC);
        fprintf(fout, "%-16s %8d\n", "version",      LM_GGML_FILE_VERSION);
        fprintf(fout, "%-16s %8d\n", "leafs",        cgraph->n_leafs);
        fprintf(fout, "%-16s %8d\n", "nodes",        cgraph->n_nodes);
        fprintf(fout, "%-16s %" PRIu64 "\n", "eval", size_eval);

        // header
        fprintf(fout, "\n");
        fprintf(fout, "%-6s %-12s %8s %8s %8s %8s %8s %16s %16s %16s %16s %16s %16s\n",
                "TYPE", "OP", "NDIMS", "NE0", "NE1", "NE2", "NE3", "NB0", "NB1", "NB2", "NB3", "DATA", "NAME");

        for (int i = 0; i < cgraph->n_leafs; ++i) {
            lm_ggml_graph_export_leaf(cgraph->leafs[i], fout);

            LM_GGML_ASSERT(cgraph->leafs[i]->op   == LM_GGML_OP_NONE);
            LM_GGML_ASSERT(cgraph->leafs[i]->src[0] == NULL);
            LM_GGML_ASSERT(cgraph->leafs[i]->src[1] == NULL);
        }

        // header
        fprintf(fout, "\n");
        fprintf(fout, "%-6s %-6s %-12s %8s %8s %8s %8s %8s %16s %16s %16s %16s %8s %16s %16s\n",
                "ARG", "TYPE", "OP", "NDIMS", "NE0", "NE1", "NE2", "NE3", "NB0", "NB1", "NB2", "NB3", "NTASKS", "DATA", "NAME");

        for (int i = 0; i < cgraph->n_nodes; ++i) {
            lm_ggml_graph_export_node(cgraph->nodes[i], "DST", fout);

            for (int j = 0; j < LM_GGML_MAX_SRC; ++j) {
                if (cgraph->nodes[i]->src[j]) {
                    lm_ggml_graph_export_node(cgraph->nodes[i]->src[j], "SRC", fout);
                }
            }

            fprintf(fout, "\n");
        }

        fprintf(fout, "\n");
    }

    // write binary data
    {
        FILE * fout = fopen(fname, "wb");

        if (!fout) {
            fprintf(stderr, "%s: failed to open %s\n", __func__, fname);
            return;
        }

        // header
        {
            const uint32_t magic   = LM_GGML_FILE_MAGIC;
            const uint32_t version = LM_GGML_FILE_VERSION;
            const uint32_t n_leafs = cgraph->n_leafs;
            const uint32_t nodes   = cgraph->n_nodes;

            fwrite(&magic,     sizeof(uint32_t), 1, fout);
            fwrite(&version,   sizeof(uint32_t), 1, fout);
            fwrite(&n_leafs,   sizeof(uint32_t), 1, fout);
            fwrite(&nodes,     sizeof(uint32_t), 1, fout);
            fwrite(&size_eval, sizeof(uint64_t), 1, fout);
        }

        // leafs
        {
            for (int i = 0; i < cgraph->n_leafs; ++i) {
                const struct lm_ggml_tensor * tensor = cgraph->leafs[i];

                const uint32_t type   = tensor->type;
                const uint32_t op     = tensor->op;
                const uint32_t n_dims = tensor->n_dims;

                fwrite(&type,   sizeof(uint32_t), 1, fout);
                fwrite(&op,     sizeof(uint32_t), 1, fout);
                fwrite(&n_dims, sizeof(uint32_t), 1, fout);

                for (int j = 0; j < LM_GGML_MAX_DIMS; ++j) {
                    const uint64_t ne = tensor->ne[j];
                    const uint64_t nb = tensor->nb[j];

                    fwrite(&ne, sizeof(uint64_t), 1, fout);
                    fwrite(&nb, sizeof(uint64_t), 1, fout);
                }

                fwrite(tensor->name,      sizeof(char), LM_GGML_MAX_NAME,      fout);
                fwrite(tensor->op_params, sizeof(char), LM_GGML_MAX_OP_PARAMS, fout);

                // dump the data
                // TODO: pad this to 32 byte boundary
                {
                    const size_t size = lm_ggml_nbytes(tensor);

                    fwrite(tensor->data, sizeof(char), size, fout);
                }
            }
        }

        // nodes
        {
            for (int i = 0; i < cgraph->n_nodes; ++i) {
                const struct lm_ggml_tensor * tensor = cgraph->nodes[i];

                const uint32_t type   = tensor->type;
                const uint32_t op     = tensor->op;
                const uint32_t n_dims = tensor->n_dims;

                fwrite(&type,   sizeof(uint32_t), 1, fout);
                fwrite(&op,     sizeof(uint32_t), 1, fout);
                fwrite(&n_dims, sizeof(uint32_t), 1, fout);

                for (int j = 0; j < LM_GGML_MAX_DIMS; ++j) {
                    const uint64_t ne = tensor->ne[j];
                    const uint64_t nb = tensor->nb[j];

                    fwrite(&ne, sizeof(uint64_t), 1, fout);
                    fwrite(&nb, sizeof(uint64_t), 1, fout);
                }

                fwrite(tensor->name,      sizeof(char), LM_GGML_MAX_NAME,      fout);
                fwrite(tensor->op_params, sizeof(char), LM_GGML_MAX_OP_PARAMS, fout);

                // output the op arguments
                {
                    struct lm_ggml_tensor * args[LM_GGML_MAX_SRC] = { NULL };

                    for (int j = 0; j < LM_GGML_MAX_SRC; ++j) {
                        args[j] = tensor->src[j];
                    }

                    for (int j = 0; j < LM_GGML_MAX_SRC; ++j) {
                        if (args[j]) {
                            int32_t idx = -1;

                            // check if leaf
                            {
                                for (int k = 0; k < cgraph->n_leafs; ++k) {
                                    if (args[j] == cgraph->leafs[k]) {
                                        idx = k;
                                        break;
                                    }
                                }
                            }

                            // check if node
                            if (idx == -1) {
                                for (int k = 0; k < cgraph->n_nodes; ++k) {
                                    if (args[j] == cgraph->nodes[k]) {
                                        idx = LM_GGML_MAX_NODES + k;
                                        break;
                                    }
                                }
                            }

                            if (idx == -1) {
                                fprintf(stderr, "%s: failed to find tensor, arg = %d, node = %d\n", __func__, j, i);
                                fclose(fout);
                                return;
                            }

                            fwrite(&idx, sizeof(int32_t), 1, fout);
                        } else {
                            const int32_t nul = -1;

                            fwrite(&nul, sizeof(int32_t), 1, fout);
                        }
                    }
                }
            }
        }

        fclose(fout);
    }
}

struct lm_ggml_cgraph lm_ggml_graph_import(const char * fname, struct lm_ggml_context ** ctx_data, struct lm_ggml_context ** ctx_eval) {
    assert(*ctx_data == NULL);
    assert(*ctx_eval == NULL);

    struct lm_ggml_cgraph result = { 0 };

    struct lm_ggml_tensor * data = NULL;

    // read file into data
    {
        FILE * fin = fopen(fname, "rb");
        if (!fin) {
            fprintf(stderr, "%s: failed to open %s\n", __func__, fname);
            return result;
        }

        size_t fsize = 0;

        fseek(fin, 0, SEEK_END);
        fsize = ftell(fin);
        fseek(fin, 0, SEEK_SET);

        // create the data context
        {
            const size_t overhead = 1*lm_ggml_tensor_overhead();

            struct lm_ggml_init_params params = {
                .mem_size   = fsize + overhead,
                .mem_buffer = NULL,
                .no_alloc   = false,
            };

            *ctx_data = lm_ggml_init(params);

            if (!*ctx_data) {
                fprintf(stderr, "%s: failed to create ggml context\n", __func__);
                fclose(fin);
                return result;
            }
        }

        data = lm_ggml_new_tensor_1d(*ctx_data, LM_GGML_TYPE_I8, fsize);

        {
            const size_t ret = fread(data->data, sizeof(char), fsize, fin);
            if (ret != fsize) {
                fprintf(stderr, "%s: failed to read %s\n", __func__, fname);
                fclose(fin);
                return result;
            }
        }

        fclose(fin);
    }

    // populate result
    {
        char * ptr = (char *) data->data;

        const uint32_t magic = *(const uint32_t *) ptr; ptr += sizeof(magic);

        if (magic != LM_GGML_FILE_MAGIC) {
            fprintf(stderr, "%s: invalid magic number, got %08x\n", __func__, magic);
            return result;
        }

        const uint32_t version = *(const uint32_t *) ptr; ptr += sizeof(version);

        if (version != LM_GGML_FILE_VERSION) {
            fprintf(stderr, "%s: invalid version number\n", __func__);
            return result;
        }

        const uint32_t n_leafs   = *(const uint32_t *) ptr; ptr += sizeof(n_leafs);
        const uint32_t n_nodes   = *(const uint32_t *) ptr; ptr += sizeof(n_nodes);
        const uint64_t size_eval = *(const uint64_t *) ptr; ptr += sizeof(size_eval);

        result.n_leafs = n_leafs;
        result.n_nodes = n_nodes;

        // create the data context
        {
            const size_t overhead = (n_leafs + n_nodes)*lm_ggml_tensor_overhead();

            struct lm_ggml_init_params params = {
                .mem_size   = size_eval + overhead,
                .mem_buffer = NULL,
                .no_alloc   = true,
            };

            *ctx_eval = lm_ggml_init(params);

            if (!*ctx_eval) {
                fprintf(stderr, "%s: failed to create ggml context\n", __func__);
                return result;
            }
        }

        // leafs
        {
            uint32_t type;
            uint32_t op;
            uint32_t n_dims;

            for (uint32_t i = 0; i < n_leafs; ++i) {
                type   = *(const uint32_t *) ptr; ptr += sizeof(type);
                op     = *(const uint32_t *) ptr; ptr += sizeof(op);
                n_dims = *(const uint32_t *) ptr; ptr += sizeof(n_dims);

                int64_t ne[LM_GGML_MAX_DIMS];
                size_t  nb[LM_GGML_MAX_DIMS];

                for (int j = 0; j < LM_GGML_MAX_DIMS; ++j) {
                    uint64_t ne_cur;
                    uint64_t nb_cur;

                    ne_cur = *(const uint64_t *) ptr; ptr += sizeof(ne_cur);
                    nb_cur = *(const uint64_t *) ptr; ptr += sizeof(nb_cur);

                    ne[j] = ne_cur;
                    nb[j] = nb_cur;
                }

                struct lm_ggml_tensor * tensor = lm_ggml_new_tensor(*ctx_eval, (enum lm_ggml_type) type, n_dims, ne);

                tensor->op = (enum lm_ggml_op) op;

                memcpy(tensor->name,      ptr, LM_GGML_MAX_NAME);      ptr += LM_GGML_MAX_NAME;
                memcpy(tensor->op_params, ptr, LM_GGML_MAX_OP_PARAMS); ptr += LM_GGML_MAX_OP_PARAMS;

                tensor->data = (void *) ptr;

                for (int j = 0; j < LM_GGML_MAX_DIMS; ++j) {
                    tensor->nb[j] = nb[j];
                }

                result.leafs[i] = tensor;

                ptr += lm_ggml_nbytes(tensor);

                fprintf(stderr, "%s: loaded leaf %d: '%16s', %3d dims, %9zu bytes\n", __func__, i, tensor->name, n_dims, lm_ggml_nbytes(tensor));
            }
        }

        lm_ggml_set_no_alloc(*ctx_eval, false);

        // nodes
        {
            uint32_t type;
            uint32_t op;
            uint32_t n_dims;

            for (uint32_t i = 0; i < n_nodes; ++i) {
                type   = *(const uint32_t *) ptr; ptr += sizeof(type);
                op     = *(const uint32_t *) ptr; ptr += sizeof(op);
                n_dims = *(const uint32_t *) ptr; ptr += sizeof(n_dims);

                enum lm_ggml_op eop = (enum lm_ggml_op) op;

                int64_t ne[LM_GGML_MAX_DIMS];
                size_t  nb[LM_GGML_MAX_DIMS];

                for (int j = 0; j < LM_GGML_MAX_DIMS; ++j) {
                    uint64_t ne_cur;
                    uint64_t nb_cur;

                    ne_cur = *(const uint64_t *) ptr; ptr += sizeof(ne_cur);
                    nb_cur = *(const uint64_t *) ptr; ptr += sizeof(nb_cur);

                    ne[j] = ne_cur;
                    nb[j] = nb_cur;
                }

                const char * ptr_name      = ptr; ptr += LM_GGML_MAX_NAME;
                const char * ptr_op_params = ptr; ptr += LM_GGML_MAX_OP_PARAMS;

                const int32_t * ptr_arg_idx = (const int32_t *) ptr; ptr += LM_GGML_MAX_SRC*sizeof(int32_t);

                struct lm_ggml_tensor * args[LM_GGML_MAX_SRC] = { NULL };

                // parse args
                for (int j = 0; j < LM_GGML_MAX_SRC; ++j) {
                    const int32_t arg_idx = ptr_arg_idx[j];

                    if (arg_idx == -1) {
                        continue;
                    }

                    if (arg_idx < LM_GGML_MAX_NODES) {
                        args[j] = result.leafs[arg_idx];
                    } else {
                        args[j] = result.nodes[arg_idx - LM_GGML_MAX_NODES];
                    }
                }

                // create the tensor
                // "view" operations are handled differently
                // TODO: handle inplace ops - currently a copy is always made

                struct lm_ggml_tensor * tensor = NULL;

                switch (eop) {
                    // TODO: implement other view ops
                    case LM_GGML_OP_RESHAPE:
                        {
                            tensor = lm_ggml_reshape_4d(*ctx_eval, args[0], ne[0], ne[1], ne[2], ne[3]);
                        } break;
                    case LM_GGML_OP_VIEW:
                        {
                            tensor = lm_ggml_view_4d(*ctx_eval, args[0], ne[0], ne[1], ne[2], ne[3], 0, 0, 0, 0);

                            size_t offs;
                            memcpy(&offs, ptr_op_params, sizeof(offs));

                            tensor->data = ((char *) tensor->data) + offs;
                        } break;
                    case LM_GGML_OP_TRANSPOSE:
                        {
                            tensor = lm_ggml_transpose(*ctx_eval, args[0]);
                        } break;
                    case LM_GGML_OP_PERMUTE:
                        {
                            tensor = lm_ggml_view_4d(*ctx_eval, args[0], ne[0], ne[1], ne[2], ne[3], 0, 0, 0, 0);
                        } break;
                    default:
                        {
                            tensor = lm_ggml_new_tensor(*ctx_eval, (enum lm_ggml_type) type, n_dims, ne);

                            tensor->op = eop;
                        } break;
                }

                memcpy(tensor->name,      ptr_name,      LM_GGML_MAX_NAME);
                memcpy(tensor->op_params, ptr_op_params, LM_GGML_MAX_OP_PARAMS);

                for (int j = 0; j < LM_GGML_MAX_DIMS; ++j) {
                    tensor->nb[j] = nb[j];
                }

                for (int j = 0; j < LM_GGML_MAX_SRC; ++j) {
                    tensor->src[j] = args[j];
                }

                result.nodes[i] = tensor;

                fprintf(stderr, "%s: loaded node %d: '%16s', %3d dims, %9zu bytes\n", __func__, i, tensor->name, n_dims, lm_ggml_nbytes(tensor));
            }
        }
    }

    return result;
}

void lm_ggml_graph_print(const struct lm_ggml_cgraph * cgraph) {
    int64_t perf_total_per_op_us[LM_GGML_OP_COUNT] = {0};

    LM_GGML_PRINT("=== GRAPH ===\n");

    LM_GGML_PRINT("n_nodes = %d\n", cgraph->n_nodes);
    for (int i = 0; i < cgraph->n_nodes; i++) {
        struct lm_ggml_tensor * node = cgraph->nodes[i];

        perf_total_per_op_us[node->op] += MAX(1, node->perf_time_us);

        LM_GGML_PRINT(" - %3d: [ %5" PRId64 ", %5" PRId64 ", %5" PRId64 "] %16s %s (%3d) cpu = %7.3f / %7.3f ms, wall = %7.3f / %7.3f ms\n",
                i,
                node->ne[0], node->ne[1], node->ne[2],
                lm_ggml_op_name(node->op), node->is_param ? "x" : node->grad ? "g" : " ", node->perf_runs,
                (double) node->perf_cycles  / (double) lm_ggml_cycles_per_ms(),
                (double) node->perf_cycles  / (double) lm_ggml_cycles_per_ms() / (double) node->perf_runs,
                (double) node->perf_time_us / 1000.0,
                (double) node->perf_time_us / 1000.0 / node->perf_runs);
    }

    LM_GGML_PRINT("n_leafs = %d\n", cgraph->n_leafs);
    for (int i = 0; i < cgraph->n_leafs; i++) {
        struct lm_ggml_tensor * node = cgraph->leafs[i];

        LM_GGML_PRINT(" - %3d: [ %5" PRId64 ", %5" PRId64 "] %8s %16s\n",
                i,
                node->ne[0], node->ne[1],
                lm_ggml_op_name(node->op),
                lm_ggml_get_name(node));
    }

    for (int i = 0; i < LM_GGML_OP_COUNT; i++) {
        if (perf_total_per_op_us[i] == 0) {
            continue;
        }

        LM_GGML_PRINT("perf_total_per_op_us[%16s] = %7.3f ms\n", lm_ggml_op_name(i), (double) perf_total_per_op_us[i] / 1000.0);
    }

    LM_GGML_PRINT("========================================\n");
}

// check if node is part of the graph
static bool lm_ggml_graph_find(const struct lm_ggml_cgraph * cgraph, const struct lm_ggml_tensor * node) {
    if (cgraph == NULL) {
        return true;
    }

    for (int i = 0; i < cgraph->n_nodes; i++) {
        if (cgraph->nodes[i] == node) {
            return true;
        }
    }

    return false;
}

static struct lm_ggml_tensor * lm_ggml_graph_get_parent(const struct lm_ggml_cgraph * cgraph, const struct lm_ggml_tensor * node) {
    for (int i = 0; i < cgraph->n_nodes; i++) {
        struct lm_ggml_tensor * parent = cgraph->nodes[i];

        if (parent->grad == node) {
            return parent;
        }
    }

    return NULL;
}

static void lm_ggml_graph_dump_dot_node_edge(FILE * fp, const struct lm_ggml_cgraph * gb, struct lm_ggml_tensor * node, struct lm_ggml_tensor * parent, const char * label)  {
    struct lm_ggml_tensor * gparent = lm_ggml_graph_get_parent(gb, node);
    struct lm_ggml_tensor * gparent0 = lm_ggml_graph_get_parent(gb, parent);
    fprintf(fp, "  \"%p\":%s -> \"%p\":%s [ arrowhead = %s; style = %s; label = \"%s\"; ]\n",
            gparent0 ? (void *) gparent0 : (void *) parent,
            gparent0 ? "g" : "x",
            gparent ? (void *) gparent : (void *) node,
            gparent ? "g" : "x",
            gparent ? "empty" : "vee",
            gparent ? "dashed" : "solid",
            label);
}

static void lm_ggml_graph_dump_dot_leaf_edge(FILE * fp, struct lm_ggml_tensor * node, struct lm_ggml_tensor * parent, const char * label)  {
    fprintf(fp, "  \"%p\":%s -> \"%p\":%s [ label = \"%s\"; ]\n",
            (void *) parent, "x",
            (void *) node, "x",
            label);
}

void lm_ggml_graph_dump_dot(const struct lm_ggml_cgraph * gb, const struct lm_ggml_cgraph * gf, const char * filename) {
    char color[16];

    FILE * fp = fopen(filename, "w");
    LM_GGML_ASSERT(fp);

    fprintf(fp, "digraph G {\n");
    fprintf(fp, "  newrank = true;\n");
    fprintf(fp, "  rankdir = LR;\n");

    for (int i = 0; i < gb->n_nodes; i++) {
        struct lm_ggml_tensor * node = gb->nodes[i];

        if (lm_ggml_graph_get_parent(gb, node) != NULL) {
            continue;
        }

        if (node->is_param) {
            snprintf(color, sizeof(color), "yellow");
        } else if (node->grad) {
            if (lm_ggml_graph_find(gf, node)) {
                snprintf(color, sizeof(color), "green");
            } else {
                snprintf(color, sizeof(color), "lightblue");
            }
        } else {
            snprintf(color, sizeof(color), "white");
        }

        fprintf(fp, "  \"%p\" [ "
                    "style = filled; fillcolor = %s; shape = record; "
                    "label=\"",
                (void *) node, color);

        if (strlen(node->name) > 0) {
            fprintf(fp, "%s (%s)|", node->name, lm_ggml_type_name(node->type));
        } else {
            fprintf(fp, "(%s)|", lm_ggml_type_name(node->type));
        }

        if (node->n_dims == 2) {
            fprintf(fp, "%d [%" PRId64 ", %" PRId64 "] | <x>%s", i, node->ne[0], node->ne[1], lm_ggml_op_symbol(node->op));
        } else {
            fprintf(fp, "%d [%" PRId64 ", %" PRId64 ", %" PRId64 "] | <x>%s", i, node->ne[0], node->ne[1], node->ne[2], lm_ggml_op_symbol(node->op));
        }

        if (node->grad) {
            fprintf(fp, " | <g>%s\"; ]\n", lm_ggml_op_symbol(node->grad->op));
        } else {
            fprintf(fp, "\"; ]\n");
        }
    }

    for (int i = 0; i < gb->n_leafs; i++) {
        struct lm_ggml_tensor * node = gb->leafs[i];

        snprintf(color, sizeof(color), "pink");

        fprintf(fp, "  \"%p\" [ "
                    "style = filled; fillcolor = %s; shape = record; "
                    "label=\"<x>",
                (void *) node, color);

        if (strlen(node->name) > 0) {
            fprintf(fp, "%s (%s)|", node->name, lm_ggml_type_name(node->type));
        } else {
            fprintf(fp, "(%s)|", lm_ggml_type_name(node->type));
        }

        fprintf(fp, "CONST %d [%" PRId64 ", %" PRId64 "]", i, node->ne[0], node->ne[1]);
        if (lm_ggml_nelements(node) < 5) {
            fprintf(fp, " | (");
            for (int j = 0; j < lm_ggml_nelements(node); j++) {
                if (node->type == LM_GGML_TYPE_I8 || node->type == LM_GGML_TYPE_I16 || node->type == LM_GGML_TYPE_I32) {
                    fprintf(fp, "%d", lm_ggml_get_i32_1d(node, j));
                }
                else if (node->type == LM_GGML_TYPE_F32 || node->type == LM_GGML_TYPE_F16) {
                    fprintf(fp, "%.1e", (double)lm_ggml_get_f32_1d(node, j));
                }
                else {
                    fprintf(fp, "#");
                }
                if (j < lm_ggml_nelements(node) - 1) {
                    fprintf(fp, ", ");
                }
            }
            fprintf(fp, ")");
        }
        fprintf(fp, "\"; ]\n");
    }

    for (int i = 0; i < gb->n_nodes; i++) {
        struct lm_ggml_tensor * node = gb->nodes[i];

        for (int j = 0; j < LM_GGML_MAX_SRC; j++) {
            if (node->src[j]) {
                char label[16];
                snprintf(label, sizeof(label), "src %d", j);
                lm_ggml_graph_dump_dot_node_edge(fp, gb, node, node->src[j], label);
            }
        }
    }

    for (int i = 0; i < gb->n_leafs; i++) {
        struct lm_ggml_tensor * node = gb->leafs[i];

        for (int j = 0; j < LM_GGML_MAX_SRC; j++) {
            if (node->src[j]) {
                char label[16];
                snprintf(label, sizeof(label), "src %d", j);
                lm_ggml_graph_dump_dot_leaf_edge(fp, node, node->src[j], label);
            }
        }
    }

    fprintf(fp, "}\n");

    fclose(fp);

    LM_GGML_PRINT("%s: dot -Tpng %s -o %s.png && open %s.png\n", __func__, filename, filename, filename);
}

////////////////////////////////////////////////////////////////////////////////

static void lm_ggml_opt_set_params(int np, struct lm_ggml_tensor * const ps[], const float * x) {
    int i = 0;
    for (int p = 0; p < np; ++p) {
        const int64_t ne = lm_ggml_nelements(ps[p]) ;
        // TODO: add function to set tensor from array
        for (int64_t j = 0; j < ne; ++j) {
            lm_ggml_set_f32_1d(ps[p], j, x[i++]);
        }
    }
}

static void lm_ggml_opt_get_params(int np, struct lm_ggml_tensor * const ps[], float * x) {
    int i = 0;
    for (int p = 0; p < np; ++p) {
        const int64_t ne = lm_ggml_nelements(ps[p]) ;
        // TODO: add function to get all elements at once
        for (int64_t j = 0; j < ne; ++j) {
            x[i++] = lm_ggml_get_f32_1d(ps[p], j);
        }
    }
}

static void lm_ggml_opt_get_grad(int np, struct lm_ggml_tensor * const ps[], float * g) {
    int64_t i = 0;
    for (int p = 0; p < np; ++p) {
        const int64_t ne = lm_ggml_nelements(ps[p]) ;
        // TODO: add function to get all elements at once
        for (int64_t j = 0; j < ne; ++j) {
            g[i++] = lm_ggml_get_f32_1d(ps[p]->grad, j);
        }
    }
}

static void lm_ggml_opt_acc_grad(int np, struct lm_ggml_tensor * const ps[], float * g, float scale) {
    int64_t i = 0;
    for (int p = 0; p < np; ++p) {
        const int64_t ne = lm_ggml_nelements(ps[p]) ;
        // TODO: add function to get all elements at once
        for (int64_t j = 0; j < ne; ++j) {
            g[i++] += lm_ggml_get_f32_1d(ps[p]->grad, j) * scale;
        }
    }
}

//
// ADAM
//
//   ref: https://arxiv.org/pdf/1412.6980.pdf
//

static enum lm_ggml_opt_result lm_ggml_opt_adam(
        struct lm_ggml_context * ctx,
        struct lm_ggml_opt_context * opt,
        struct lm_ggml_opt_params params,
        struct lm_ggml_tensor * f,
        struct lm_ggml_cgraph * gf,
        struct lm_ggml_cgraph * gb,
        lm_ggml_opt_callback callback,
        void * callback_data) {
    LM_GGML_ASSERT(lm_ggml_is_scalar(f));

    // these will store the parameters we want to optimize
    struct lm_ggml_tensor * ps[LM_GGML_MAX_PARAMS];

    int np = 0;
    int64_t nx = 0;
    for (int i = 0; i < gf->n_nodes; ++i) {
        if (gf->nodes[i]->is_param) {
            LM_GGML_PRINT_DEBUG("found param %d: grad->op = %d\n", np, gf->nodes[i]->grad->op);

            LM_GGML_ASSERT(np < LM_GGML_MAX_PARAMS);

            ps[np++] = gf->nodes[i];
            nx += lm_ggml_nelements(gf->nodes[i]);
        }
    }

    if ((opt->params.type != params.type) || (opt->nx != nx) || (opt->params.past != params.past)) {
        int iter = opt->iter;
        lm_ggml_opt_init(opt->ctx, opt, params, nx);
        opt->iter = iter;
    }

    // constants
    float sched = params.adam.sched;
    const float alpha = params.adam.alpha;
    const float decay = params.adam.decay * alpha;
    const float beta1 = params.adam.beta1;
    const float beta2 = params.adam.beta2;
    const float eps   = params.adam.eps;
    const float gclip = params.adam.gclip;
    const int decay_min_ndim = params.adam.decay_min_ndim;
    const int n_accum = MAX(1, params.n_gradient_accumulation);
    const float accum_norm = 1.0f / (float) n_accum;

    float * g  = opt->adam.g->data;  // gradients
    float * m  = opt->adam.m->data;  // first moment
    float * v  = opt->adam.v->data;  // second moment

    float * pf = params.past > 0 ? opt->adam.pf->data : NULL; // past function values

    struct lm_ggml_cplan cplan = lm_ggml_graph_plan(gb, params.n_threads);
    struct lm_ggml_object * obj = lm_ggml_new_object(ctx, LM_GGML_OBJECT_WORK_BUFFER, cplan.work_size);
    cplan.work_data = (uint8_t *)ctx->mem_buffer + obj->offs;

    bool cancel = false;

    // compute the function value
    float fx = 0;
    lm_ggml_set_zero(opt->adam.g);
    for (int accum_step = 0; accum_step < n_accum; ++accum_step) {
        if (callback) {
            callback(callback_data, accum_step, &sched, &cancel);
            if (cancel) {
                return LM_GGML_OPT_CANCEL;
            }
        }
        // lm_ggml_graph_reset  (gf);
        lm_ggml_set_f32      (f->grad, 1.0f);
        lm_ggml_graph_compute(gb, &cplan);
        lm_ggml_opt_acc_grad(np, ps, g, accum_norm);
        fx += lm_ggml_get_f32_1d(f, 0);
    }
    fx *= accum_norm;

    opt->adam.fx_prev = fx;
    opt->adam.fx_best = opt->adam.fx_prev;
    if (pf) {
        pf[opt->iter % params.past] = opt->adam.fx_prev;
    }

    opt->loss_before = opt->adam.fx_prev;
    opt->loss_after  = opt->adam.fx_prev;

    // initialize
    if (opt->just_initialized) {
        opt->adam.n_no_improvement = 0;
        opt->just_initialized = false;
    }

    float * fx_best = &opt->adam.fx_best;
    float * fx_prev = &opt->adam.fx_prev;
    int * n_no_improvement = &opt->adam.n_no_improvement;

    int iter0 = opt->iter;

    // run the optimizer
    for (int t = 0; t < params.adam.n_iter; ++t) {
        opt->iter = iter0 + t + 1;
        LM_GGML_PRINT_DEBUG  ("=== iter %d ===\n", t);

        LM_GGML_PRINT_DEBUG  ("f      = %10.6f\n", lm_ggml_get_f32_1d(f, 0));
        LM_GGML_PRINT_DEBUG_5("df/dx0 = %10.6f\n", lm_ggml_get_f32_1d(ps[0]->grad, 0));
        LM_GGML_PRINT_DEBUG_5("df/dx1 = %10.6f\n", lm_ggml_get_f32_1d(ps[1]->grad, 0));

        for (int i = 0; i < np; ++i) {
            LM_GGML_PRINT_DEBUG("param %d: %10.6f, g = %10.6f\n", i,
                    lm_ggml_get_f32_1d(ps[i], 0), lm_ggml_get_f32_1d(ps[i]->grad, 0));
        }

        const int64_t t_start_wall = lm_ggml_time_us();
        const int64_t t_start_cpu = lm_ggml_cycles();
        UNUSED(t_start_wall);
        UNUSED(t_start_cpu);

        {
            float gnorm = 1.0f;
            if (gclip > 0.0f) {
                // gradient clipping
                lm_ggml_float sum = 0.0;
                for (int64_t i = 0; i < nx; ++i) {
                    sum += (lm_ggml_float)(g[i]*g[i]);
                }
                lm_ggml_float norm = sqrt(sum);
                if (norm > (lm_ggml_float) gclip) {
                    gnorm = (float) ((lm_ggml_float) gclip / norm);
                }
            }
            const float beta1h = alpha*sched/(1.0f - powf(beta1, opt->iter));
            const float beta2h =        1.0f/(1.0f - powf(beta2, opt->iter));
            int64_t i = 0;
            for (int p = 0; p < np; ++p) {
                const int64_t ne = lm_ggml_nelements(ps[p]);
                const float p_decay = ((ps[p]->n_dims >= decay_min_ndim) ? decay : 0.0f) * sched;
                for (int64_t j = 0; j < ne; ++j) {
                    float x  = lm_ggml_get_f32_1d(ps[p], j);
                    float g_ = g[i]*gnorm;
                    m[i] = m[i]*beta1 +    g_*(1.0f - beta1);
                    v[i] = v[i]*beta2 + g_*g_*(1.0f - beta2);
                    float mh = m[i]*beta1h;
                    float vh = v[i]*beta2h;
                    vh = sqrtf(vh) + eps;
                    x  = x*(1.0f - p_decay) - mh/vh;
                    lm_ggml_set_f32_1d(ps[p], j, x);
                    ++i;
                }
            }
        }

        fx = 0;
        lm_ggml_set_zero(opt->adam.g);
        for (int accum_step = 0; accum_step < n_accum; ++accum_step) {
            if (callback) {
                callback(callback_data, accum_step, &sched, &cancel);
                if (cancel) {
                    return LM_GGML_OPT_CANCEL;;
                }
            }
            // lm_ggml_graph_reset  (gf);
            lm_ggml_set_f32      (f->grad, 1.0f);
            lm_ggml_graph_compute(gb, &cplan);
            lm_ggml_opt_acc_grad(np, ps, g, accum_norm);
            fx += lm_ggml_get_f32_1d(f, 0);
        }
        fx *= accum_norm;

        opt->loss_after = fx;

        // check convergence
        if (fabsf(fx - fx_prev[0])/fx < params.adam.eps_f) {
            LM_GGML_PRINT_DEBUG("converged\n");

            return LM_GGML_OPT_OK;
        }

        // delta-based convergence test
        if (pf != NULL) {
            // need at least params.past iterations to start checking for convergence
            if (params.past <= iter0 + t) {
                const float rate = (pf[(iter0 + t)%params.past] - fx)/fx;

                if (fabsf(rate) < params.delta) {
                    return LM_GGML_OPT_OK;
                }
            }

            pf[(iter0 + t)%params.past] = fx;
        }

        // check for improvement
        if (params.max_no_improvement > 0) {
            if (fx_best[0] > fx) {
                fx_best[0] = fx;
                n_no_improvement[0] = 0;
            } else {
                ++n_no_improvement[0];

                if (n_no_improvement[0] >= params.max_no_improvement) {
                    return LM_GGML_OPT_OK;
                }
            }
        }

        fx_prev[0] = fx;

        {
            const int64_t t_end_cpu = lm_ggml_cycles();
            LM_GGML_PRINT_DEBUG("time iter:      %5.3f s\n", ((float)(t_end_cpu - t_start_cpu))/CLOCKS_PER_SEC);
            UNUSED(t_end_cpu);

            const int64_t t_end_wall = lm_ggml_time_us();
            LM_GGML_PRINT_DEBUG("wall time iter: %5.3f s\n", (t_end_wall - t_start_wall)/1e6);
            UNUSED(t_end_wall);
        }
    }

    return LM_GGML_OPT_DID_NOT_CONVERGE;
}

//
// L-BFGS
//
// the L-BFGS implementation below is based on the following implementation:
//
//   https://github.com/chokkan/liblbfgs
//

struct lm_ggml_lbfgs_iteration_data {
    float alpha;
    float ys;
    float * s;
    float * y;
};

static enum lm_ggml_opt_result linesearch_backtracking(
        const struct lm_ggml_opt_params * params,
        int nx,
        float * x,
        float * fx,
        float * g,
        float * d,
        float * step,
        const float * xp,
        struct lm_ggml_tensor * f,
        struct lm_ggml_cgraph * gb,
        struct lm_ggml_cplan  * cplan,
        const int np,
        struct lm_ggml_tensor * ps[],
        bool * cancel,
        lm_ggml_opt_callback callback,
        void * callback_data) {
    int count = 0;

    float width  = 0.0f;
    float dg     = 0.0f;
    float finit  = 0.0f;
    float dginit = 0.0f;
    float dgtest = 0.0f;

    const float dec = 0.5f;
    const float inc = 2.1f;

    const int n_accum = MAX(1, params->n_gradient_accumulation);
    const float accum_norm = 1.0f / (float) n_accum;

    if (*step <= 0.f) {
        return LM_GGML_LINESEARCH_INVALID_PARAMETERS;
    }

    // compute the initial gradient in the search direction
    lm_ggml_vec_dot_f32(nx, &dginit, g, d);

    // make sure that d points to a descent direction
    if (0 < dginit) {
        return LM_GGML_LINESEARCH_FAIL;
    }

    // initialize local variables
    finit = *fx;
    dgtest = params->lbfgs.ftol*dginit;

    while (true) {
        lm_ggml_vec_cpy_f32(nx, x, xp);
        lm_ggml_vec_mad_f32(nx, x, d, *step);

        // evaluate the function and gradient values
        {
            lm_ggml_opt_set_params(np, ps, x);

            *fx = 0;
            memset(g, 0, sizeof(float)*nx);
            for (int accum_step = 0; accum_step < n_accum; ++accum_step) {
                if (callback) {
                    // LBFG-S does not support learning rate -> ignore learning schedule
                    float sched = 0;
                    callback(callback_data, accum_step, &sched, cancel);
                    if (*cancel) {
                        return LM_GGML_OPT_CANCEL;
                    }
                }
                // lm_ggml_graph_reset  (gf);
                lm_ggml_set_f32      (f->grad, 1.0f);
                lm_ggml_graph_compute(gb, cplan);
                lm_ggml_opt_acc_grad(np, ps, g, accum_norm);
                *fx += lm_ggml_get_f32_1d(f, 0);
            }
            *fx *= accum_norm;

        }

        ++count;

        if (*fx > finit + (*step)*dgtest) {
            width = dec;
        } else {
            // Armijo condition is satisfied
            if (params->lbfgs.linesearch == LM_GGML_LINESEARCH_BACKTRACKING_ARMIJO) {
                return count;
            }

            lm_ggml_vec_dot_f32(nx, &dg, g, d);

            // check the Wolfe condition
            if (dg < params->lbfgs.wolfe * dginit) {
                width = inc;
            } else {
                if(params->lbfgs.linesearch == LM_GGML_LINESEARCH_BACKTRACKING_WOLFE) {
                    // regular Wolfe conditions
                    return count;
                }

                if(dg > -params->lbfgs.wolfe*dginit) {
                    width = dec;
                } else {
                    // strong Wolfe condition (LM_GGML_LINESEARCH_BACKTRACKING_STRONG_WOLFE)
                    return count;
                }
            }
        }

        if (*step < params->lbfgs.min_step) {
            return LM_GGML_LINESEARCH_MINIMUM_STEP;
        }
        if (*step > params->lbfgs.max_step) {
            return LM_GGML_LINESEARCH_MAXIMUM_STEP;
        }
        if (params->lbfgs.max_linesearch <= count) {
            return LM_GGML_LINESEARCH_MAXIMUM_ITERATIONS;
        }

        (*step) *= width;
    }

    LM_GGML_UNREACHABLE();
}

static enum lm_ggml_opt_result lm_ggml_opt_lbfgs(
        struct lm_ggml_context * ctx,
        struct lm_ggml_opt_context * opt,
        struct lm_ggml_opt_params params,
        struct lm_ggml_tensor * f,
        struct lm_ggml_cgraph * gf,
        struct lm_ggml_cgraph * gb,
        lm_ggml_opt_callback callback,
        void * callback_data) {
    if (params.lbfgs.linesearch == LM_GGML_LINESEARCH_BACKTRACKING_WOLFE ||
        params.lbfgs.linesearch == LM_GGML_LINESEARCH_BACKTRACKING_STRONG_WOLFE) {
        if (params.lbfgs.wolfe <= params.lbfgs.ftol || 1.f <= params.lbfgs.wolfe) {
            return LM_GGML_OPT_INVALID_WOLFE;
        }
    }

    const int m = params.lbfgs.m;

    // these will store the parameters we want to optimize
    struct lm_ggml_tensor * ps[LM_GGML_MAX_PARAMS];

    int np = 0;
    int nx = 0;
    for (int i = 0; i < gf->n_nodes; ++i) {
        if (gf->nodes[i]->is_param) {
            LM_GGML_PRINT_DEBUG("found param %d: grad->op = %d\n", np, gf->nodes[i]->grad->op);

            LM_GGML_ASSERT(np < LM_GGML_MAX_PARAMS);

            ps[np++] = gf->nodes[i];
            nx += lm_ggml_nelements(gf->nodes[i]);
        }
    }

    if ((opt->params.type != params.type) || (opt->nx != nx) || (opt->params.past != params.past) || (opt->params.lbfgs.m != params.lbfgs.m)) {
        int iter = opt->iter;
        lm_ggml_opt_init(ctx, opt, params, nx);
        opt->iter = iter;
    }

    struct lm_ggml_cplan cplan = lm_ggml_graph_plan(gb, params.n_threads);
    struct lm_ggml_object * obj = lm_ggml_new_object(ctx, LM_GGML_OBJECT_WORK_BUFFER, cplan.work_size);
    cplan.work_data = (uint8_t *)ctx->mem_buffer + obj->offs;

    float * x  = opt->lbfgs.x->data;  // current parameters
    float * xp = opt->lbfgs.xp->data; // previous parameters
    float * g  = opt->lbfgs.g->data;  // current gradient
    float * gp = opt->lbfgs.gp->data; // previous gradient
    float * d  = opt->lbfgs.d->data;  // search direction

    float * pf = params.past > 0 ? opt->lbfgs.pf->data : NULL; // past function values

    const int n_accum = MAX(1, params.n_gradient_accumulation);
    const float accum_norm = 1.0f / (float) n_accum;

    float fx    = 0.0f; // cost function value
    float xnorm = 0.0f; // ||x||
    float gnorm = 0.0f; // ||g||

    // initialize x from the graph nodes
    lm_ggml_opt_get_params(np, ps, x);

    // the L-BFGS memory
    float * lm_alpha = opt->lbfgs.lmal->data;
    float * lm_ys    = opt->lbfgs.lmys->data;
    float * lm_s     = opt->lbfgs.lms->data;
    float * lm_y     = opt->lbfgs.lmy->data;

    bool cancel = false;

    // evaluate the function value and its gradient
    {
        lm_ggml_opt_set_params(np, ps, x);

        fx = 0;
        memset(g, 0, sizeof(float)*nx);
        for (int accum_step = 0; accum_step < n_accum; ++accum_step) {
            if (callback) {
                // LBFG-S does not support learning rate -> ignore learning schedule
                float sched = 0;
                callback(callback_data, accum_step, &sched, &cancel);
                if (cancel) {
                    return LM_GGML_OPT_CANCEL;
                }
            }
            // lm_ggml_graph_reset  (gf);
            lm_ggml_set_f32      (f->grad, 1.0f);
            lm_ggml_graph_compute(gb, &cplan);
            lm_ggml_opt_acc_grad(np, ps, g, accum_norm);
            fx += lm_ggml_get_f32_1d(f, 0);
        }
        fx *= accum_norm;

        opt->loss_before = fx;
        opt->loss_after  = fx;
    }

    // search direction = -gradient
    lm_ggml_vec_neg_f32(nx, d, g);

    // ||x||, ||g||
    lm_ggml_vec_norm_f32(nx, &xnorm, x);
    lm_ggml_vec_norm_f32(nx, &gnorm, g);

    if (xnorm < 1.0f) {
        xnorm = 1.0f;
    }

    // already optimized
    if (gnorm/xnorm <= params.lbfgs.eps) {
        return LM_GGML_OPT_OK;
    }

    if (opt->just_initialized) {
        if (pf) {
            pf[0] = fx;
        }
        opt->lbfgs.fx_best = fx;

        // initial step
        lm_ggml_vec_norm_inv_f32(nx, &opt->lbfgs.step, d);
        opt->lbfgs.j                = 0;
        opt->lbfgs.k                = 1;
        opt->lbfgs.end              = 0;
        opt->lbfgs.n_no_improvement = 0;
        opt->just_initialized       = false;
    }

    float * fx_best        = &opt->lbfgs.fx_best;
    float * step           = &opt->lbfgs.step;
    int * j                = &opt->lbfgs.j;
    int * k                = &opt->lbfgs.k;
    int * end              = &opt->lbfgs.end;
    int * n_no_improvement = &opt->lbfgs.n_no_improvement;

    int ls     = 0;
    int bound  = 0;

    float ys   = 0.0f;
    float yy   = 0.0f;
    float beta = 0.0f;

    int it = 0;

    while (true) {
        // store the current position and gradient vectors
        lm_ggml_vec_cpy_f32(nx, xp, x);
        lm_ggml_vec_cpy_f32(nx, gp, g);

        // TODO: instead of passing &cancel here, use the return code of the linesearch
        //       to determine if the optimization should be cancelled
        //       this is a simple change, but not doing this atm, since I don't have a nice
        //       way to test and don't want to break something with so many changes lined up
        ls = linesearch_backtracking(&params, nx, x, &fx, g, d, step, xp, f, gb, &cplan, np, ps, &cancel, callback, callback_data);
        if (cancel) {
            return LM_GGML_OPT_CANCEL;
        }

        if (ls < 0) {
            // linesearch failed - go back to the previous point and return
            lm_ggml_vec_cpy_f32(nx, x, xp);
            lm_ggml_vec_cpy_f32(nx, g, gp);

            return ls;
        }

        opt->loss_after = fx;

        lm_ggml_vec_norm_f32(nx, &xnorm, x);
        lm_ggml_vec_norm_f32(nx, &gnorm, g);

        LM_GGML_PRINT_DEBUG("f = %10.6f\n", lm_ggml_get_f32_1d(f, 0));

        if (xnorm < 1.0f) {
            xnorm = 1.0f;
        }
        if (gnorm/xnorm <= params.lbfgs.eps) {
            // converged
            return LM_GGML_OPT_OK;
        }

        // delta-based convergence test
        if (pf != NULL) {
            // need at least params.past iterations to start checking for convergence
            if (params.past <= k[0]) {
                const float rate = (pf[k[0]%params.past] - fx)/fx;

                if (fabsf(rate) < params.delta) {
                    return LM_GGML_OPT_OK;
                }
            }

            pf[k[0]%params.past] = fx;
        }

        // check for improvement
        if (params.max_no_improvement > 0) {
            if (fx < fx_best[0]) {
                fx_best[0] = fx;
                n_no_improvement[0] = 0;
            } else {
                n_no_improvement[0]++;

                if (n_no_improvement[0] >= params.max_no_improvement) {
                    return LM_GGML_OPT_OK;
                }
            }
        }

        if (params.lbfgs.n_iter != 0 && params.lbfgs.n_iter < it + 1) {
            // reached the maximum number of iterations
            return LM_GGML_OPT_DID_NOT_CONVERGE;
        }

        // update vectors s and y:
        //   s_{k+1} = x_{k+1} - x_{k} = \step * d_{k}.
        //   y_{k+1} = g_{k+1} - g_{k}.
        //
        lm_ggml_vec_sub_f32(nx, &lm_s[end[0]*nx], x, xp);
        lm_ggml_vec_sub_f32(nx, &lm_y[end[0]*nx], g, gp);

        // compute scalars ys and yy:
        //     ys = y^t \cdot s    -> 1 / \rho.
        //     yy = y^t \cdot y.
        //
        lm_ggml_vec_dot_f32(nx, &ys, &lm_y[end[0]*nx], &lm_s[end[0]*nx]);
        lm_ggml_vec_dot_f32(nx, &yy, &lm_y[end[0]*nx], &lm_y[end[0]*nx]);

        lm_ys[end[0]] = ys;

        // find new search direction
        //   ref: https://en.wikipedia.org/wiki/Limited-memory_BFGS

        bound = (m <= k[0]) ? m : k[0];
        k[0]++;
        it++;
        end[0] = (end[0] + 1)%m;

        // initialize search direction with -g
        lm_ggml_vec_neg_f32(nx, d, g);

        j[0] = end[0];
        for (int i = 0; i < bound; ++i) {
            j[0] = (j[0] + m - 1) % m;
            // \alpha_{j} = \rho_{j} s^{t}_{j} \cdot q_{k+1}
            lm_ggml_vec_dot_f32(nx, &lm_alpha[j[0]], &lm_s[j[0]*nx], d);
            lm_alpha[j[0]] /= lm_ys[j[0]];
            // q_{i} = q_{i+1} - \alpha_{i} y_{i}
            lm_ggml_vec_mad_f32(nx, d, &lm_y[j[0]*nx], -lm_alpha[j[0]]);
        }

        lm_ggml_vec_scale_f32(nx, d, ys/yy);

        for (int i = 0; i < bound; ++i) {
            // \beta_{j} = \rho_{j} y^t_{j} \cdot \gamma_{i}
            lm_ggml_vec_dot_f32(nx, &beta, &lm_y[j[0]*nx], d);
            beta /= lm_ys[j[0]];
            // \gamma_{i+1} = \gamma_{i} + (\alpha_{j} - \beta_{j}) s_{j}
            lm_ggml_vec_mad_f32(nx, d, &lm_s[j[0]*nx], lm_alpha[j[0]] - beta);
            j[0] = (j[0] + 1)%m;
        }

        step[0] = 1.0;
    }

    LM_GGML_UNREACHABLE();
}

struct lm_ggml_opt_params lm_ggml_opt_default_params(enum lm_ggml_opt_type type) {
    struct lm_ggml_opt_params result;

    switch (type) {
        case LM_GGML_OPT_ADAM:
            {
                result = (struct lm_ggml_opt_params) {
                    .type      = LM_GGML_OPT_ADAM,
                    .n_threads = 1,
                    .past      = 0,
                    .delta     = 1e-5f,

                    .max_no_improvement = 100,

                    .print_forward_graph  = true,
                    .print_backward_graph = true,

                    .n_gradient_accumulation = 1,

                    .adam = {
                        .n_iter = 10000,
                        .sched  = 1.000f,
                        .decay  = 0.0f,
                        .decay_min_ndim = 2,
                        .alpha  = 0.001f,
                        .beta1  = 0.9f,
                        .beta2  = 0.999f,
                        .eps    = 1e-8f,
                        .eps_f  = 1e-5f,
                        .eps_g  = 1e-3f,
                        .gclip  = 0.0f,
                    },
                };
            } break;
        case LM_GGML_OPT_LBFGS:
            {
                result = (struct lm_ggml_opt_params) {
                    .type      = LM_GGML_OPT_LBFGS,
                    .n_threads = 1,
                    .past      = 0,
                    .delta     = 1e-5f,

                    .max_no_improvement = 0,

                    .print_forward_graph  = true,
                    .print_backward_graph = true,

                    .n_gradient_accumulation = 1,

                    .lbfgs = {
                        .m              = 6,
                        .n_iter         = 100,
                        .max_linesearch = 20,

                        .eps      = 1e-5f,
                        .ftol     = 1e-4f,
                        .wolfe    = 0.9f,
                        .min_step = 1e-20f,
                        .max_step = 1e+20f,

                        .linesearch = LM_GGML_LINESEARCH_DEFAULT,
                    },
                };
            } break;
    }

    return result;
}

LM_GGML_API void lm_ggml_opt_init(
        struct lm_ggml_context * ctx,
        struct lm_ggml_opt_context * opt,
        struct lm_ggml_opt_params params,
        int64_t nx) {
    opt->ctx = ctx;
    opt->params = params;
    opt->iter = 0;
    opt->nx = nx;
    opt->just_initialized = true;
    if (opt->ctx == NULL) {
        struct lm_ggml_init_params ctx_opt_params;
        if (opt->params.type == LM_GGML_OPT_ADAM) {
            ctx_opt_params.mem_size = LM_GGML_MEM_ALIGN*3 + lm_ggml_tensor_overhead()*3 + lm_ggml_type_size(LM_GGML_TYPE_F32)*nx*3;
            if (opt->params.past > 0) {
                ctx_opt_params.mem_size += LM_GGML_MEM_ALIGN + lm_ggml_tensor_overhead() + lm_ggml_type_size(LM_GGML_TYPE_F32)*opt->params.past;
            }
        } else if (opt->params.type == LM_GGML_OPT_LBFGS) {
            ctx_opt_params.mem_size = LM_GGML_MEM_ALIGN*9 + lm_ggml_tensor_overhead()*9 + lm_ggml_type_size(LM_GGML_TYPE_F32)*(nx*5 + opt->params.lbfgs.m*2 + nx*opt->params.lbfgs.m*2);
            if (opt->params.past > 0) {
                ctx_opt_params.mem_size += LM_GGML_MEM_ALIGN + lm_ggml_tensor_overhead() + lm_ggml_type_size(LM_GGML_TYPE_F32)*opt->params.past;
            }
        }
        ctx_opt_params.mem_buffer = NULL;
        ctx_opt_params.no_alloc   = false;

        opt->ctx = lm_ggml_init(ctx_opt_params);
    }
    switch (opt->params.type) {
        case LM_GGML_OPT_ADAM:
            {
                opt->adam.g  = lm_ggml_new_tensor_1d(opt->ctx, LM_GGML_TYPE_F32, nx);
                opt->adam.m  = lm_ggml_new_tensor_1d(opt->ctx, LM_GGML_TYPE_F32, nx);
                opt->adam.v  = lm_ggml_new_tensor_1d(opt->ctx, LM_GGML_TYPE_F32, nx);
                opt->adam.pf = params.past > 0
                    ? lm_ggml_new_tensor_1d(opt->ctx, LM_GGML_TYPE_F32, params.past)
                    : NULL;
                lm_ggml_set_zero(opt->adam.m);
                lm_ggml_set_zero(opt->adam.v);
                if (opt->adam.pf) {
                    lm_ggml_set_zero(opt->adam.pf);
                }
            } break;
        case LM_GGML_OPT_LBFGS:
            {
                opt->lbfgs.x  = lm_ggml_new_tensor_1d(opt->ctx, LM_GGML_TYPE_F32, nx);
                opt->lbfgs.xp = lm_ggml_new_tensor_1d(opt->ctx, LM_GGML_TYPE_F32, nx);
                opt->lbfgs.g  = lm_ggml_new_tensor_1d(opt->ctx, LM_GGML_TYPE_F32, nx);
                opt->lbfgs.gp = lm_ggml_new_tensor_1d(opt->ctx, LM_GGML_TYPE_F32, nx);
                opt->lbfgs.d  = lm_ggml_new_tensor_1d(opt->ctx, LM_GGML_TYPE_F32, nx);
                opt->lbfgs.pf = params.past > 0
                    ? lm_ggml_new_tensor_1d(opt->ctx, LM_GGML_TYPE_F32, params.past)
                    : NULL;
                opt->lbfgs.lmal = lm_ggml_new_tensor_1d(opt->ctx, LM_GGML_TYPE_F32, params.lbfgs.m);
                opt->lbfgs.lmys = lm_ggml_new_tensor_1d(opt->ctx, LM_GGML_TYPE_F32, params.lbfgs.m);
                opt->lbfgs.lms  = lm_ggml_new_tensor_2d(opt->ctx, LM_GGML_TYPE_F32, nx, params.lbfgs.m);
                opt->lbfgs.lmy  = lm_ggml_new_tensor_2d(opt->ctx, LM_GGML_TYPE_F32, nx, params.lbfgs.m);
                lm_ggml_set_zero(opt->lbfgs.x);
                lm_ggml_set_zero(opt->lbfgs.xp);
                lm_ggml_set_zero(opt->lbfgs.g);
                lm_ggml_set_zero(opt->lbfgs.gp);
                lm_ggml_set_zero(opt->lbfgs.d);
                if (opt->lbfgs.pf) {
                    lm_ggml_set_zero(opt->lbfgs.pf);
                }
                lm_ggml_set_zero(opt->lbfgs.lmal);
                lm_ggml_set_zero(opt->lbfgs.lmys);
                lm_ggml_set_zero(opt->lbfgs.lms);
                lm_ggml_set_zero(opt->lbfgs.lmy);
            } break;
    }
}

enum lm_ggml_opt_result lm_ggml_opt(
        struct lm_ggml_context * ctx,
        struct lm_ggml_opt_params params,
        struct lm_ggml_tensor * f) {
    bool free_ctx = false;
    if (ctx == NULL) {
        struct lm_ggml_init_params params_ctx = {
            .mem_size   = 16*1024*1024,
            .mem_buffer = NULL,
            .no_alloc   = false,
        };

        ctx = lm_ggml_init(params_ctx);
        if (ctx == NULL) {
            return LM_GGML_OPT_NO_CONTEXT;
        }

        free_ctx = true;
    }

    enum lm_ggml_opt_result result = LM_GGML_OPT_OK;

    struct lm_ggml_opt_context * opt = (struct lm_ggml_opt_context *) alloca(sizeof(struct lm_ggml_opt_context));

    lm_ggml_opt_init(ctx, opt, params, 0);
    result = lm_ggml_opt_resume(ctx, opt, f);

    if (free_ctx) {
        lm_ggml_free(ctx);
    }

    return result;
}

enum lm_ggml_opt_result lm_ggml_opt_resume(
        struct lm_ggml_context * ctx,
        struct lm_ggml_opt_context * opt,
        struct lm_ggml_tensor * f) {

    // build forward + backward compute graphs
    struct lm_ggml_tensor * gfbuf = lm_ggml_new_tensor_1d(ctx, LM_GGML_TYPE_I32, sizeof(struct lm_ggml_cgraph) / lm_ggml_type_size(LM_GGML_TYPE_I32)+ (sizeof(struct lm_ggml_cgraph) % lm_ggml_type_size(LM_GGML_TYPE_I32) ? 1 : 0));
    struct lm_ggml_tensor * gbbuf = lm_ggml_new_tensor_1d(ctx, LM_GGML_TYPE_I32, sizeof(struct lm_ggml_cgraph) / lm_ggml_type_size(LM_GGML_TYPE_I32)+ (sizeof(struct lm_ggml_cgraph) % lm_ggml_type_size(LM_GGML_TYPE_I32) ? 1 : 0));

    struct lm_ggml_cgraph * gf = (struct lm_ggml_cgraph *) gfbuf->data;
    struct lm_ggml_cgraph * gb = (struct lm_ggml_cgraph *) gbbuf->data;

    *gf = lm_ggml_build_forward (f);
    *gb = lm_ggml_build_backward(ctx, gf, true);

    return lm_ggml_opt_resume_g(ctx, opt, f, gf, gb, NULL, NULL);
}

enum lm_ggml_opt_result lm_ggml_opt_resume_g(
        struct lm_ggml_context * ctx,
        struct lm_ggml_opt_context * opt,
        struct lm_ggml_tensor * f,
        struct lm_ggml_cgraph * gf,
        struct lm_ggml_cgraph * gb,
        lm_ggml_opt_callback callback,
        void * callback_data) {

    // build forward + backward compute graphs
    enum lm_ggml_opt_result result = LM_GGML_OPT_OK;

    switch (opt->params.type) {
        case LM_GGML_OPT_ADAM:
            {
                result = lm_ggml_opt_adam(ctx, opt, opt->params, f, gf, gb, callback, callback_data);
            } break;
        case LM_GGML_OPT_LBFGS:
            {
                result = lm_ggml_opt_lbfgs(ctx, opt, opt->params, f, gf, gb, callback, callback_data);
            } break;
    }

    if (opt->params.print_forward_graph) {
        lm_ggml_graph_print   (gf);
        lm_ggml_graph_dump_dot(gf, NULL, "opt-forward.dot");
    }

    if (opt->params.print_backward_graph) {
        lm_ggml_graph_print   (gb);
        lm_ggml_graph_dump_dot(gb, gf, "opt-backward.dot");
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

size_t lm_ggml_quantize_q4_0(const float * src, void * dst, int n, int k, int64_t * hist) {
    assert(k % QK4_0 == 0);
    const int nb = k / QK4_0;

    for (int b = 0; b < n; b += k) {
        block_q4_0 * restrict y = (block_q4_0 *) dst + b/QK4_0;

        quantize_row_q4_0_reference(src + b, y, k);

        for (int i = 0; i < nb; i++) {
            for (int j = 0; j < QK4_0; j += 2) {
                const uint8_t vi0 = y[i].qs[j/2] & 0x0F;
                const uint8_t vi1 = y[i].qs[j/2] >> 4;

                hist[vi0]++;
                hist[vi1]++;
            }
        }
    }

    return (n/QK4_0*sizeof(block_q4_0));
}

size_t lm_ggml_quantize_q4_1(const float * src, void * dst, int n, int k, int64_t * hist) {
    assert(k % QK4_1 == 0);
    const int nb = k / QK4_1;

    for (int b = 0; b < n; b += k) {
        block_q4_1 * restrict y = (block_q4_1 *) dst + b/QK4_1;

        quantize_row_q4_1_reference(src + b, y, k);

        for (int i = 0; i < nb; i++) {
            for (int j = 0; j < QK4_1; j += 2) {
                const uint8_t vi0 = y[i].qs[j/2] & 0x0F;
                const uint8_t vi1 = y[i].qs[j/2] >> 4;

                hist[vi0]++;
                hist[vi1]++;
            }
        }
    }

    return (n/QK4_1*sizeof(block_q4_1));
}

size_t lm_ggml_quantize_q5_0(const float * src, void * dst, int n, int k, int64_t * hist) {
    assert(k % QK5_0 == 0);
    const int nb = k / QK5_0;

    for (int b = 0; b < n; b += k) {
        block_q5_0 * restrict y = (block_q5_0 *)dst + b/QK5_0;

        quantize_row_q5_0_reference(src + b, y, k);

        for (int i = 0; i < nb; i++) {
            uint32_t qh;
            memcpy(&qh, &y[i].qh, sizeof(qh));

            for (int j = 0; j < QK5_0; j += 2) {
                const uint8_t vh0 = ((qh & (1u << (j + 0 ))) >> (j + 0 )) << 4;
                const uint8_t vh1 = ((qh & (1u << (j + 16))) >> (j + 12));

                // cast to 16 bins
                const uint8_t vi0 = ((y[i].qs[j/2] & 0x0F) | vh0) / 2;
                const uint8_t vi1 = ((y[i].qs[j/2] >>   4) | vh1) / 2;

                hist[vi0]++;
                hist[vi1]++;
            }
        }
    }

    return (n/QK5_0*sizeof(block_q5_0));
}

size_t lm_ggml_quantize_q5_1(const float * src, void * dst, int n, int k, int64_t * hist) {
    assert(k % QK5_1 == 0);
    const int nb = k / QK5_1;

    for (int b = 0; b < n; b += k) {
        block_q5_1 * restrict y = (block_q5_1 *)dst + b/QK5_1;

        quantize_row_q5_1_reference(src + b, y, k);

        for (int i = 0; i < nb; i++) {
            uint32_t qh;
            memcpy(&qh, &y[i].qh, sizeof(qh));

            for (int j = 0; j < QK5_1; j += 2) {
                const uint8_t vh0 = ((qh & (1u << (j + 0 ))) >> (j + 0 )) << 4;
                const uint8_t vh1 = ((qh & (1u << (j + 16))) >> (j + 12));

                // cast to 16 bins
                const uint8_t vi0 = ((y[i].qs[j/2] & 0x0F) | vh0) / 2;
                const uint8_t vi1 = ((y[i].qs[j/2] >>   4) | vh1) / 2;

                hist[vi0]++;
                hist[vi1]++;
            }
        }
    }

    return (n/QK5_1*sizeof(block_q5_1));
}

size_t lm_ggml_quantize_q8_0(const float * src, void * dst, int n, int k, int64_t * hist) {
    assert(k % QK8_0 == 0);
    const int nb = k / QK8_0;

    for (int b = 0; b < n; b += k) {
        block_q8_0 * restrict y = (block_q8_0 *)dst + b/QK8_0;

        quantize_row_q8_0_reference(src + b, y, k);

        for (int i = 0; i < nb; i++) {
            for (int j = 0; j < QK8_0; ++j) {
                const int8_t vi = y[i].qs[j];

                hist[vi/16 + 8]++;
            }
        }
    }

    return (n/QK8_0*sizeof(block_q8_0));
}

size_t lm_ggml_quantize_chunk(enum lm_ggml_type type, const float * src, void * dst, int start, int n, int64_t * hist) {
    size_t result = 0;
    switch (type) {
        case LM_GGML_TYPE_Q4_0:
            {
                LM_GGML_ASSERT(start % QK4_0 == 0);
                block_q4_0 * block = (block_q4_0*)dst + start / QK4_0;
                result = lm_ggml_quantize_q4_0(src + start, block, n, n, hist);
            } break;
        case LM_GGML_TYPE_Q4_1:
            {
                LM_GGML_ASSERT(start % QK4_1 == 0);
                block_q4_1 * block = (block_q4_1*)dst + start / QK4_1;
                result = lm_ggml_quantize_q4_1(src + start, block, n, n, hist);
            } break;
        case LM_GGML_TYPE_Q5_0:
            {
                LM_GGML_ASSERT(start % QK5_0 == 0);
                block_q5_0 * block = (block_q5_0*)dst + start / QK5_0;
                result = lm_ggml_quantize_q5_0(src + start, block, n, n, hist);
            } break;
        case LM_GGML_TYPE_Q5_1:
            {
                LM_GGML_ASSERT(start % QK5_1 == 0);
                block_q5_1 * block = (block_q5_1*)dst + start / QK5_1;
                result = lm_ggml_quantize_q5_1(src + start, block, n, n, hist);
            } break;
        case LM_GGML_TYPE_Q8_0:
            {
                LM_GGML_ASSERT(start % QK8_0 == 0);
                block_q8_0 * block = (block_q8_0*)dst + start / QK8_0;
                result = lm_ggml_quantize_q8_0(src + start, block, n, n, hist);
            } break;
#ifdef LM_GGML_USE_K_QUANTS
        case LM_GGML_TYPE_Q2_K:
            {
                LM_GGML_ASSERT(start % QK_K == 0);
                block_q2_K * block = (block_q2_K*)dst + start / QK_K;
                result = lm_ggml_quantize_q2_K(src + start, block, n, n, hist);
            } break;
        case LM_GGML_TYPE_Q3_K:
            {
                LM_GGML_ASSERT(start % QK_K == 0);
                block_q3_K * block = (block_q3_K*)dst + start / QK_K;
                result = lm_ggml_quantize_q3_K(src + start, block, n, n, hist);
            } break;
        case LM_GGML_TYPE_Q4_K:
            {
                LM_GGML_ASSERT(start % QK_K == 0);
                block_q4_K * block = (block_q4_K*)dst + start / QK_K;
                result = lm_ggml_quantize_q4_K(src + start, block, n, n, hist);
            } break;
        case LM_GGML_TYPE_Q5_K:
            {
                LM_GGML_ASSERT(start % QK_K == 0);
                block_q5_K * block = (block_q5_K*)dst + start / QK_K;
                result = lm_ggml_quantize_q5_K(src + start, block, n, n, hist);
            } break;
        case LM_GGML_TYPE_Q6_K:
            {
                LM_GGML_ASSERT(start % QK_K == 0);
                block_q6_K * block = (block_q6_K*)dst + start / QK_K;
                result = lm_ggml_quantize_q6_K(src + start, block, n, n, hist);
            } break;
#endif
        case LM_GGML_TYPE_F16:
            {
                int elemsize = sizeof(lm_ggml_fp16_t);
                lm_ggml_fp32_to_fp16_row(src + start, (lm_ggml_fp16_t *)dst + start, n);
                result = n * elemsize;
            } break;
        case LM_GGML_TYPE_F32:
            {
                int elemsize = sizeof(float);
                result = n * elemsize;
                memcpy((uint8_t *)dst + start * elemsize, src + start, result);
            } break;
        default:
            assert(false);
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

struct lm_gguf_str {
    uint64_t n;  // GGUFv2
    char * data;
};

static const size_t LM_GGUF_TYPE_SIZE[LM_GGUF_TYPE_COUNT] = {
    [LM_GGUF_TYPE_UINT8]   = sizeof(uint8_t),
    [LM_GGUF_TYPE_INT8]    = sizeof(int8_t),
    [LM_GGUF_TYPE_UINT16]  = sizeof(uint16_t),
    [LM_GGUF_TYPE_INT16]   = sizeof(int16_t),
    [LM_GGUF_TYPE_UINT32]  = sizeof(uint32_t),
    [LM_GGUF_TYPE_INT32]   = sizeof(int32_t),
    [LM_GGUF_TYPE_FLOAT32] = sizeof(float),
    [LM_GGUF_TYPE_BOOL]    = sizeof(bool),
    [LM_GGUF_TYPE_STRING]  = sizeof(struct lm_gguf_str),
    [LM_GGUF_TYPE_UINT64]  = sizeof(uint64_t),
    [LM_GGUF_TYPE_INT64]   = sizeof(int64_t),
    [LM_GGUF_TYPE_FLOAT64] = sizeof(double),
    [LM_GGUF_TYPE_ARRAY]   = 0, // undefined
};
static_assert(LM_GGUF_TYPE_COUNT == 13, "LM_GGUF_TYPE_COUNT != 13");

static const char * LM_GGUF_TYPE_NAME[LM_GGUF_TYPE_COUNT] = {
    [LM_GGUF_TYPE_UINT8]   = "u8",
    [LM_GGUF_TYPE_INT8]    = "i8",
    [LM_GGUF_TYPE_UINT16]  = "u16",
    [LM_GGUF_TYPE_INT16]   = "i16",
    [LM_GGUF_TYPE_UINT32]  = "u32",
    [LM_GGUF_TYPE_INT32]   = "i32",
    [LM_GGUF_TYPE_FLOAT32] = "f32",
    [LM_GGUF_TYPE_BOOL]    = "bool",
    [LM_GGUF_TYPE_STRING]  = "str",
    [LM_GGUF_TYPE_ARRAY]   = "arr",
    [LM_GGUF_TYPE_UINT64]  = "u64",
    [LM_GGUF_TYPE_INT64]   = "i64",
    [LM_GGUF_TYPE_FLOAT64] = "f64",
};
static_assert(LM_GGUF_TYPE_COUNT == 13, "LM_GGUF_TYPE_COUNT != 13");

union lm_gguf_value {
    uint8_t  uint8;
    int8_t   int8;
    uint16_t uint16;
    int16_t  int16;
    uint32_t uint32;
    int32_t  int32;
    float    float32;
    uint64_t uint64;
    int64_t  int64;
    double   float64;
    bool     bool_;

    struct lm_gguf_str str;

    struct {
        enum lm_gguf_type type;

        uint64_t n;  // GGUFv2
        void * data;
    } arr;
};

struct lm_gguf_kv {
    struct lm_gguf_str key;

    enum  lm_gguf_type  type;
    union lm_gguf_value value;
};

struct lm_gguf_header {
    char magic[4];
    uint32_t version;
    uint64_t n_tensors; // GGUFv2
    uint64_t n_kv;      // GGUFv2
};

struct lm_gguf_tensor_info {
    struct lm_gguf_str name;

    uint32_t n_dims;
    uint64_t ne[LM_GGML_MAX_DIMS];

    enum lm_ggml_type type;

    uint64_t offset; // offset from start of `data`, must be a multiple of `ALIGNMENT`

    // for writing API
    const void * data;
    size_t size;
};

struct lm_gguf_context {
    struct lm_gguf_header header;

    struct lm_gguf_kv          * kv;
    struct lm_gguf_tensor_info * infos;

    size_t alignment;
    size_t offset;    // offset of `data` from beginning of file
    size_t size;      // size of `data` in bytes

    //uint8_t * padding;
    void * data;
};

static bool lm_gguf_fread_el(FILE * file, void * dst, size_t size, size_t * offset) {
    const size_t n = fread(dst, 1, size, file);
    *offset += n;
    return n == size;
}

// NOTE: temporary handling of GGUFv1 >> remove after Oct 2023
static bool lm_gguf_fread_str_cur(FILE * file, struct lm_gguf_str * p, size_t * offset) {
    p->n    = 0;
    p->data = NULL;

    bool ok = true;

    ok = ok && lm_gguf_fread_el(file, &p->n,    sizeof(p->n), offset); p->data = calloc(p->n + 1, 1);
    ok = ok && lm_gguf_fread_el(file,  p->data, p->n,         offset);

    return ok;
}

static bool lm_gguf_fread_str_v1(FILE * file, struct lm_gguf_str * p, size_t * offset) {
    p->n    = 0;
    p->data = NULL;

    bool ok = true;

    uint32_t n = 0;
    ok = ok && lm_gguf_fread_el(file, &n,       sizeof(n), offset); p->data = calloc(n + 1, 1); p->n = n;
    ok = ok && lm_gguf_fread_el(file,  p->data, p->n,      offset);

    return ok;
}

struct lm_gguf_context * lm_gguf_init_empty(void) {
    struct lm_gguf_context * ctx = LM_GGML_ALIGNED_MALLOC(sizeof(struct lm_gguf_context));

    memcpy(ctx->header.magic, LM_GGUF_MAGIC, sizeof(ctx->header.magic));
    ctx->header.version   = LM_GGUF_VERSION;
    ctx->header.n_tensors = 0;
    ctx->header.n_kv      = 0;

    ctx->kv    = NULL;
    ctx->infos = NULL;

    ctx->alignment = LM_GGUF_DEFAULT_ALIGNMENT;
    ctx->offset    = 0;
    ctx->size      = 0;

    ctx->data = NULL;

    return ctx;
}

struct lm_gguf_context * lm_gguf_init_from_file(const char * fname, struct lm_gguf_init_params params) {
    FILE * file = fopen(fname, "rb");
    if (!file) {
        return NULL;
    }

    // offset from start of file
    size_t offset = 0;

    char magic[4];

    // check the magic before making allocations
    {
        lm_gguf_fread_el(file, &magic, sizeof(magic), &offset);

        for (uint32_t i = 0; i < sizeof(magic); i++) {
            if (magic[i] != LM_GGUF_MAGIC[i]) {
                fprintf(stderr, "%s: invalid magic characters %s.\n", __func__, magic);
                fclose(file);
                return NULL;
            }
        }
    }

    bool ok = true;

    struct lm_gguf_context * ctx = LM_GGML_ALIGNED_MALLOC(sizeof(struct lm_gguf_context));

    // read the header
    {
        strncpy(ctx->header.magic, magic, 4);


        ctx->kv    = NULL;
        ctx->infos = NULL;
        ctx->data  = NULL;

        ok = ok && lm_gguf_fread_el(file, &ctx->header.version,   sizeof(ctx->header.version),   &offset);

        if (ctx->header.version == 1) {
            // NOTE: temporary handling of GGUFv1 >> remove after Oct 2023
            uint32_t n_tensors = 0;
            uint32_t n_kv      = 0;

            ok = ok && lm_gguf_fread_el(file, &n_tensors, sizeof(n_tensors), &offset);
            ok = ok && lm_gguf_fread_el(file, &n_kv,      sizeof(n_kv),      &offset);

            ctx->header.n_tensors = n_tensors;
            ctx->header.n_kv      = n_kv;
        } else {
            ok = ok && lm_gguf_fread_el(file, &ctx->header.n_tensors, sizeof(ctx->header.n_tensors), &offset);
            ok = ok && lm_gguf_fread_el(file, &ctx->header.n_kv,      sizeof(ctx->header.n_kv),      &offset);
        }

        if (!ok) {
            fprintf(stderr, "%s: failed to read header\n", __func__);
            fclose(file);
            lm_gguf_free(ctx);
            return NULL;
        }
    }

    // NOTE: temporary handling of GGUFv1 >> remove after Oct 2023
    bool (* lm_gguf_fread_str)(FILE *, struct lm_gguf_str *, size_t *) = lm_gguf_fread_str_cur;
    if (ctx->header.version == 1) {
        lm_gguf_fread_str = lm_gguf_fread_str_v1;
    }

    // read the kv pairs
    {
        ctx->kv = malloc(ctx->header.n_kv * sizeof(struct lm_gguf_kv));

        for (uint32_t i = 0; i < ctx->header.n_kv; ++i) {
            struct lm_gguf_kv * kv = &ctx->kv[i];

            //fprintf(stderr, "%s: reading kv %d\n", __func__, i);

            ok = ok && lm_gguf_fread_str(file, &kv->key,                    &offset);
            ok = ok && lm_gguf_fread_el (file, &kv->type, sizeof(kv->type), &offset);

            //fprintf(stderr, "%s: reading kv with key %s\n", __func__, kv->key.data);

            switch (kv->type) {
                case LM_GGUF_TYPE_UINT8:   ok = ok && lm_gguf_fread_el (file, &kv->value.uint8,   sizeof(kv->value.uint8),   &offset); break;
                case LM_GGUF_TYPE_INT8:    ok = ok && lm_gguf_fread_el (file, &kv->value.int8,    sizeof(kv->value.int8),    &offset); break;
                case LM_GGUF_TYPE_UINT16:  ok = ok && lm_gguf_fread_el (file, &kv->value.uint16,  sizeof(kv->value.uint16),  &offset); break;
                case LM_GGUF_TYPE_INT16:   ok = ok && lm_gguf_fread_el (file, &kv->value.int16,   sizeof(kv->value.int16),   &offset); break;
                case LM_GGUF_TYPE_UINT32:  ok = ok && lm_gguf_fread_el (file, &kv->value.uint32,  sizeof(kv->value.uint32),  &offset); break;
                case LM_GGUF_TYPE_INT32:   ok = ok && lm_gguf_fread_el (file, &kv->value.int32,   sizeof(kv->value.int32),   &offset); break;
                case LM_GGUF_TYPE_FLOAT32: ok = ok && lm_gguf_fread_el (file, &kv->value.float32, sizeof(kv->value.float32), &offset); break;
                case LM_GGUF_TYPE_UINT64:  ok = ok && lm_gguf_fread_el (file, &kv->value.uint64,  sizeof(kv->value.uint64),  &offset); break;
                case LM_GGUF_TYPE_INT64:   ok = ok && lm_gguf_fread_el (file, &kv->value.int64,   sizeof(kv->value.int64),   &offset); break;
                case LM_GGUF_TYPE_FLOAT64: ok = ok && lm_gguf_fread_el (file, &kv->value.float64, sizeof(kv->value.float64), &offset); break;
                case LM_GGUF_TYPE_BOOL:    ok = ok && lm_gguf_fread_el (file, &kv->value.bool_,   sizeof(kv->value.bool_),   &offset); break;
                case LM_GGUF_TYPE_STRING:  ok = ok && lm_gguf_fread_str(file, &kv->value.str,                                &offset); break;
                case LM_GGUF_TYPE_ARRAY:
                    {
                        ok = ok && lm_gguf_fread_el(file, &kv->value.arr.type, sizeof(kv->value.arr.type), &offset);

                        if (ctx->header.version == 1) {
                            // NOTE: temporary handling of GGUFv1 >> remove after Oct 2023
                            uint32_t n = 0;
                            ok = ok && lm_gguf_fread_el(file, &n, sizeof(n), &offset);
                            kv->value.arr.n = n;
                        } else {
                            ok = ok && lm_gguf_fread_el(file, &kv->value.arr.n, sizeof(kv->value.arr.n), &offset);
                        }

                        switch (kv->value.arr.type) {
                            case LM_GGUF_TYPE_UINT8:
                            case LM_GGUF_TYPE_INT8:
                            case LM_GGUF_TYPE_UINT16:
                            case LM_GGUF_TYPE_INT16:
                            case LM_GGUF_TYPE_UINT32:
                            case LM_GGUF_TYPE_INT32:
                            case LM_GGUF_TYPE_FLOAT32:
                            case LM_GGUF_TYPE_UINT64:
                            case LM_GGUF_TYPE_INT64:
                            case LM_GGUF_TYPE_FLOAT64:
                            case LM_GGUF_TYPE_BOOL:
                                {
                                    kv->value.arr.data = malloc(kv->value.arr.n * LM_GGUF_TYPE_SIZE[kv->value.arr.type]);
                                    ok = ok && lm_gguf_fread_el(file, kv->value.arr.data, kv->value.arr.n * LM_GGUF_TYPE_SIZE[kv->value.arr.type], &offset);
                                } break;
                            case LM_GGUF_TYPE_STRING:
                                {
                                    kv->value.arr.data = malloc(kv->value.arr.n * sizeof(struct lm_gguf_str));
                                    for (uint32_t j = 0; j < kv->value.arr.n; ++j) {
                                        ok = ok && lm_gguf_fread_str(file, &((struct lm_gguf_str *) kv->value.arr.data)[j], &offset);
                                    }
                                } break;
                            case LM_GGUF_TYPE_ARRAY:
                            case LM_GGUF_TYPE_COUNT: LM_GGML_ASSERT(false && "invalid type"); break;
                        }
                    } break;
                case LM_GGUF_TYPE_COUNT: LM_GGML_ASSERT(false && "invalid type");
            }

            if (!ok) {
                break;
            }
        }

        if (!ok) {
            fprintf(stderr, "%s: failed to read key-value pairs\n", __func__);
            fclose(file);
            lm_gguf_free(ctx);
            return NULL;
        }
    }

    // read the tensor infos
    {
        ctx->infos = malloc(ctx->header.n_tensors * sizeof(struct lm_gguf_tensor_info));

        for (uint32_t i = 0; i < ctx->header.n_tensors; ++i) {
            struct lm_gguf_tensor_info * info = &ctx->infos[i];

            for (int j = 0; j < LM_GGML_MAX_DIMS; ++j) {
                info->ne[j] = 1;
            }

            ok = ok && lm_gguf_fread_str(file, &info->name,                          &offset);
            ok = ok && lm_gguf_fread_el (file, &info->n_dims, sizeof(info->n_dims),  &offset);
            for (uint32_t j = 0; j < info->n_dims; ++j) {
                if (ctx->header.version == 1) {
                    // NOTE: temporary handling of GGUFv1 >> remove after Oct 2023
                    uint32_t t = 0;
                    ok = ok && lm_gguf_fread_el(file, &t, sizeof(t), &offset);
                    info->ne[j] = t;
                } else {
                    ok = ok && lm_gguf_fread_el(file, &info->ne[j], sizeof(info->ne[j]), &offset);
                }
            }
            ok = ok && lm_gguf_fread_el (file, &info->type,   sizeof(info->type),    &offset);
            ok = ok && lm_gguf_fread_el (file, &info->offset, sizeof(info->offset),  &offset);

            if (!ok) {
                fprintf(stderr, "%s: failed to read tensor info\n", __func__);
                fclose(file);
                lm_gguf_free(ctx);
                return NULL;
            }
        }
    }

    ctx->alignment = LM_GGUF_DEFAULT_ALIGNMENT;

    int alignment_idx = lm_gguf_find_key(ctx, "general.alignment");
    if (alignment_idx != -1) {
        ctx->alignment = lm_gguf_get_val_u32(ctx, alignment_idx);
    }

    // we require the data section to be aligned, so take into account any padding
    {
        const size_t offset_pad = offset % ctx->alignment;

        if (offset_pad != 0) {
            offset += ctx->alignment - offset_pad;
            fseek(file, offset, SEEK_SET);
        }
    }

    // store the current file offset - this is where the data section starts
    ctx->offset = offset;

    // compute the total size of the data section, taking into account the alignment
    {
        ctx->size = 0;
        for (uint32_t i = 0; i < ctx->header.n_tensors; ++i) {
            struct lm_gguf_tensor_info * info = &ctx->infos[i];

            const int64_t ne =
                (int64_t) info->ne[0] *
                (int64_t) info->ne[1] *
                (int64_t) info->ne[2] *
                (int64_t) info->ne[3];

            if (ne % lm_ggml_blck_size(info->type) != 0) {
                fprintf(stderr, "%s: tensor '%s' number of elements (%" PRId64 ") is not a multiple of block size (%d)\n",
                        __func__, info->name.data, ne, lm_ggml_blck_size(info->type));
                fclose(file);
                lm_gguf_free(ctx);
                return NULL;
            }

            const size_t size_cur = (ne*lm_ggml_type_size(info->type))/lm_ggml_blck_size(info->type);

            ctx->size += LM_GGML_PAD(size_cur, ctx->alignment);
        }
    }

    // load the tensor data only if requested
    if (params.ctx != NULL) {
        // if the provided lm_gguf_context is no_alloc, then we create "empty" tensors and do not read the binary blob
        // otherwise, we load the binary blob into the created lm_ggml_context as well, and point the "data" members of
        // the lm_ggml_tensor structs to the appropriate locations in the binary blob

        // compute the exact size needed for the new lm_ggml_context
        const size_t mem_size =
            params.no_alloc ?
            (ctx->header.n_tensors    )*lm_ggml_tensor_overhead() :
            (ctx->header.n_tensors + 1)*lm_ggml_tensor_overhead() + ctx->size;

        struct lm_ggml_init_params pdata = {
            .mem_size   = mem_size,
            .mem_buffer = NULL,
            .no_alloc   = params.no_alloc,
        };

        *params.ctx = lm_ggml_init(pdata);

        struct lm_ggml_context * ctx_data = *params.ctx;

        struct lm_ggml_tensor * data = NULL;

        if (!params.no_alloc) {
            data = lm_ggml_new_tensor_1d(ctx_data, LM_GGML_TYPE_I8, ctx->size);

            ok = ok && data != NULL;

            // read the binary blob with the tensor data
            ok = ok && lm_gguf_fread_el(file, data->data, ctx->size, &offset);

            if (!ok) {
                fprintf(stderr, "%s: failed to read tensor data\n", __func__);
                fclose(file);
                lm_ggml_free(ctx_data);
                lm_gguf_free(ctx);
                return NULL;
            }

            ctx->data = data->data;
        }

        lm_ggml_set_no_alloc(ctx_data, true);

        // create the tensors
        for (uint32_t i = 0; i < ctx->header.n_tensors; ++i) {
            const int64_t ne[LM_GGML_MAX_DIMS] = {
                ctx->infos[i].ne[0],
                ctx->infos[i].ne[1],
                ctx->infos[i].ne[2],
                ctx->infos[i].ne[3],
            };

            struct lm_ggml_tensor * cur = lm_ggml_new_tensor(ctx_data, ctx->infos[i].type, ctx->infos[i].n_dims, ne);

            ok = ok && cur != NULL;

            lm_ggml_set_name(cur, ctx->infos[i].name.data);

            if (!ok) {
                break;
            }

            // point the data member to the appropriate location in the binary blob using the tensor infos
            if (!params.no_alloc) {
              //cur->data = (char *) data->data + ctx->infos[i].offset - ctx->offset; // offset from start of file
                cur->data = (char *) data->data + ctx->infos[i].offset;               // offset from data
            }
        }

        if (!ok) {
            fprintf(stderr, "%s: failed to read the tensor data\n", __func__);
            fclose(file);
            lm_ggml_free(ctx_data);
            lm_gguf_free(ctx);
            return NULL;
        }

        lm_ggml_set_no_alloc(ctx_data, params.no_alloc);
    }

    fclose(file);

    return ctx;
}

void lm_gguf_free(struct lm_gguf_context * ctx) {
    if (ctx == NULL) {
        return;
    }

    if (ctx->kv) {
        // free string memory - not great..
        for (uint32_t i = 0; i < ctx->header.n_kv; ++i) {
            struct lm_gguf_kv * kv = &ctx->kv[i];

            if (kv->key.data) {
                free(kv->key.data);
            }

            if (kv->type == LM_GGUF_TYPE_STRING) {
                if (kv->value.str.data) {
                    free(kv->value.str.data);
                }
            }

            if (kv->type == LM_GGUF_TYPE_ARRAY) {
                if (kv->value.arr.data) {
                    if (kv->value.arr.type == LM_GGUF_TYPE_STRING) {
                        for (uint32_t j = 0; j < kv->value.arr.n; ++j) {
                            struct lm_gguf_str * str = &((struct lm_gguf_str *) kv->value.arr.data)[j];
                            if (str->data) {
                                free(str->data);
                            }
                        }
                    }
                    free(kv->value.arr.data);
                }
            }
        }

        free(ctx->kv);
    }

    if (ctx->infos) {
        for (uint32_t i = 0; i < ctx->header.n_tensors; ++i) {
            struct lm_gguf_tensor_info * info = &ctx->infos[i];

            if (info->name.data) {
                free(info->name.data);
            }
        }

        free(ctx->infos);
    }

    LM_GGML_ALIGNED_FREE(ctx);
}

const char * lm_gguf_type_name(enum lm_gguf_type type) {
    return LM_GGUF_TYPE_NAME[type];
}

int lm_gguf_get_version(const struct lm_gguf_context * ctx) {
    return ctx->header.version;
}

size_t lm_gguf_get_alignment(const struct lm_gguf_context * ctx) {
    return ctx->alignment;
}

size_t lm_gguf_get_data_offset(const struct lm_gguf_context * ctx) {
    return ctx->offset;
}

void * lm_gguf_get_data(const struct lm_gguf_context * ctx) {
    return ctx->data;
}

int lm_gguf_get_n_kv(const struct lm_gguf_context * ctx) {
    return ctx->header.n_kv;
}

int lm_gguf_find_key(const struct lm_gguf_context * ctx, const char * key) {
    // return -1 if key not found
    int keyfound = -1;

    const int n_kv = lm_gguf_get_n_kv(ctx);

    for (int i = 0; i < n_kv; ++i) {
        if (strcmp(key, lm_gguf_get_key(ctx, i)) == 0) {
            keyfound = i;
            break;
        }
    }

    return keyfound;
}

const char * lm_gguf_get_key(const struct lm_gguf_context * ctx, int key_id) {
    return ctx->kv[key_id].key.data;
}

enum lm_gguf_type lm_gguf_get_kv_type(const struct lm_gguf_context * ctx, int key_id) {
    return ctx->kv[key_id].type;
}

enum lm_gguf_type lm_gguf_get_arr_type(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_ARRAY);
    return ctx->kv[key_id].value.arr.type;
}

const void * lm_gguf_get_arr_data(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_ARRAY);
    return ctx->kv[key_id].value.arr.data;
}

const char * lm_gguf_get_arr_str(const struct lm_gguf_context * ctx, int key_id, int i) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_ARRAY);
    struct lm_gguf_kv * kv = &ctx->kv[key_id];
    struct lm_gguf_str * str = &((struct lm_gguf_str *) kv->value.arr.data)[i];
    return str->data;
}

int lm_gguf_get_arr_n(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_ARRAY);
    return ctx->kv[key_id].value.arr.n;
}

uint8_t lm_gguf_get_val_u8(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_UINT8);
    return ctx->kv[key_id].value.uint8;
}

int8_t lm_gguf_get_val_i8(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_INT8);
    return ctx->kv[key_id].value.int8;
}

uint16_t lm_gguf_get_val_u16(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_UINT16);
    return ctx->kv[key_id].value.uint16;
}

int16_t lm_gguf_get_val_i16(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_INT16);
    return ctx->kv[key_id].value.int16;
}

uint32_t lm_gguf_get_val_u32(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_UINT32);
    return ctx->kv[key_id].value.uint32;
}

int32_t lm_gguf_get_val_i32(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_INT32);
    return ctx->kv[key_id].value.int32;
}

float lm_gguf_get_val_f32(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_FLOAT32);
    return ctx->kv[key_id].value.float32;
}

uint64_t lm_gguf_get_val_u64(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_UINT64);
    return ctx->kv[key_id].value.uint64;
}

int64_t lm_gguf_get_val_i64(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_INT64);
    return ctx->kv[key_id].value.int64;
}

double lm_gguf_get_val_f64(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_FLOAT64);
    return ctx->kv[key_id].value.float64;
}

bool lm_gguf_get_val_bool(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_BOOL);
    return ctx->kv[key_id].value.bool_;
}

const char * lm_gguf_get_val_str(const struct lm_gguf_context * ctx, int key_id) {
    LM_GGML_ASSERT(ctx->kv[key_id].type == LM_GGUF_TYPE_STRING);
    return ctx->kv[key_id].value.str.data;
}

int lm_gguf_get_n_tensors(const struct lm_gguf_context * ctx) {
    return ctx->header.n_tensors;
}

int lm_gguf_find_tensor(const struct lm_gguf_context * ctx, const char * name) {
    // return -1 if tensor not found
    int tensorfound = -1;

    const int n_tensors = lm_gguf_get_n_tensors(ctx);

    for (int i = 0; i < n_tensors; ++i) {
        if (strcmp(name, lm_gguf_get_tensor_name(ctx, i)) == 0) {
            tensorfound = i;
            break;
        }
    }

    return tensorfound;
}

size_t lm_gguf_get_tensor_offset(const struct lm_gguf_context * ctx, int i) {
    return ctx->infos[i].offset;
}

char * lm_gguf_get_tensor_name(const struct lm_gguf_context * ctx, int i) {
    return ctx->infos[i].name.data;
}

// returns the index
static int lm_gguf_get_or_add_key(struct lm_gguf_context * ctx, const char * key) {
    const int idx = lm_gguf_find_key(ctx, key);
    if (idx >= 0) {
        return idx;
    }

    const int n_kv = lm_gguf_get_n_kv(ctx);

    ctx->kv = realloc(ctx->kv, (n_kv + 1) * sizeof(struct lm_gguf_kv));
    ctx->kv[n_kv].key.n    = strlen(key);
    ctx->kv[n_kv].key.data = strdup(key);
    ctx->header.n_kv++;

    return n_kv;
}

void lm_gguf_set_val_u8(struct lm_gguf_context * ctx, const char * key, uint8_t val) {
    const int idx = lm_gguf_get_or_add_key(ctx, key);

    ctx->kv[idx].type        = LM_GGUF_TYPE_UINT8;
    ctx->kv[idx].value.uint8 = val;
}

void lm_gguf_set_val_i8(struct lm_gguf_context * ctx, const char * key, int8_t val) {
    const int idx = lm_gguf_get_or_add_key(ctx, key);

    ctx->kv[idx].type       = LM_GGUF_TYPE_INT8;
    ctx->kv[idx].value.int8 = val;
}

void lm_gguf_set_val_u16(struct lm_gguf_context * ctx, const char * key, uint16_t val) {
    const int idx = lm_gguf_get_or_add_key(ctx, key);

    ctx->kv[idx].type         = LM_GGUF_TYPE_UINT16;
    ctx->kv[idx].value.uint16 = val;
}

void lm_gguf_set_val_i16(struct lm_gguf_context * ctx, const char * key, int16_t val) {
    const int idx = lm_gguf_get_or_add_key(ctx, key);

    ctx->kv[idx].type        = LM_GGUF_TYPE_INT16;
    ctx->kv[idx].value.int16 = val;
}

void lm_gguf_set_val_u32(struct lm_gguf_context * ctx, const char * key, uint32_t val) {
    const int idx = lm_gguf_get_or_add_key(ctx, key);

    ctx->kv[idx].type         = LM_GGUF_TYPE_UINT32;
    ctx->kv[idx].value.uint32 = val;
}

void lm_gguf_set_val_i32(struct lm_gguf_context * ctx, const char * key, int32_t val) {
    const int idx = lm_gguf_get_or_add_key(ctx, key);

    ctx->kv[idx].type        = LM_GGUF_TYPE_INT32;
    ctx->kv[idx].value.int32 = val;
}

void lm_gguf_set_val_f32(struct lm_gguf_context * ctx, const char * key, float val) {
    const int idx = lm_gguf_get_or_add_key(ctx, key);

    ctx->kv[idx].type          = LM_GGUF_TYPE_FLOAT32;
    ctx->kv[idx].value.float32 = val;
}

void lm_gguf_set_val_u64(struct lm_gguf_context * ctx, const char * key, uint64_t val) {
    const int idx = lm_gguf_get_or_add_key(ctx, key);

    ctx->kv[idx].type         = LM_GGUF_TYPE_UINT64;
    ctx->kv[idx].value.uint64 = val;
}

void lm_gguf_set_val_i64(struct lm_gguf_context * ctx, const char * key, int64_t val) {
    const int idx = lm_gguf_get_or_add_key(ctx, key);

    ctx->kv[idx].type        = LM_GGUF_TYPE_INT64;
    ctx->kv[idx].value.int64 = val;
}

void lm_gguf_set_val_f64(struct lm_gguf_context * ctx, const char * key, double val) {
    const int idx = lm_gguf_get_or_add_key(ctx, key);

    ctx->kv[idx].type          = LM_GGUF_TYPE_FLOAT64;
    ctx->kv[idx].value.float64 = val;
}

void lm_gguf_set_val_bool(struct lm_gguf_context * ctx, const char * key, bool val) {
    const int idx = lm_gguf_get_or_add_key(ctx, key);

    ctx->kv[idx].type        = LM_GGUF_TYPE_BOOL;
    ctx->kv[idx].value.bool_ = val;
}

void lm_gguf_set_val_str(struct lm_gguf_context * ctx, const char * key, const char * val) {
    const int idx = lm_gguf_get_or_add_key(ctx, key);

    ctx->kv[idx].type           = LM_GGUF_TYPE_STRING;
    ctx->kv[idx].value.str.n    = strlen(val);
    ctx->kv[idx].value.str.data = strdup(val);
}

void lm_gguf_set_arr_data(struct lm_gguf_context * ctx, const char * key, enum lm_gguf_type type, const void * data, int n) {
    const int idx = lm_gguf_get_or_add_key(ctx, key);

    ctx->kv[idx].type           = LM_GGUF_TYPE_ARRAY;
    ctx->kv[idx].value.arr.type = type;
    ctx->kv[idx].value.arr.n    = n;
    ctx->kv[idx].value.arr.data = malloc(n*LM_GGUF_TYPE_SIZE[type]);
    memcpy(ctx->kv[idx].value.arr.data, data, n*LM_GGUF_TYPE_SIZE[type]);
}

void lm_gguf_set_arr_str(struct lm_gguf_context * ctx, const char * key, const char ** data, int n) {
    const int idx = lm_gguf_get_or_add_key(ctx, key);

    ctx->kv[idx].type           = LM_GGUF_TYPE_ARRAY;
    ctx->kv[idx].value.arr.type = LM_GGUF_TYPE_STRING;
    ctx->kv[idx].value.arr.n    = n;
    ctx->kv[idx].value.arr.data = malloc(n*sizeof(struct lm_gguf_str));
    for (int i = 0; i < n; i++) {
        struct lm_gguf_str * str = &((struct lm_gguf_str *)ctx->kv[idx].value.arr.data)[i];
        str->n    = strlen(data[i]);
        str->data = strdup(data[i]);
    }
}

// set or add KV pairs from another context
void lm_gguf_set_kv(struct lm_gguf_context * ctx, struct lm_gguf_context * src) {
    for (uint32_t i = 0; i < src->header.n_kv; i++) {
        switch (src->kv[i].type) {
            case LM_GGUF_TYPE_UINT8:   lm_gguf_set_val_u8  (ctx, src->kv[i].key.data, src->kv[i].value.uint8);    break;
            case LM_GGUF_TYPE_INT8:    lm_gguf_set_val_i8  (ctx, src->kv[i].key.data, src->kv[i].value.int8);     break;
            case LM_GGUF_TYPE_UINT16:  lm_gguf_set_val_u16 (ctx, src->kv[i].key.data, src->kv[i].value.uint16);   break;
            case LM_GGUF_TYPE_INT16:   lm_gguf_set_val_i16 (ctx, src->kv[i].key.data, src->kv[i].value.int16);    break;
            case LM_GGUF_TYPE_UINT32:  lm_gguf_set_val_u32 (ctx, src->kv[i].key.data, src->kv[i].value.uint32);   break;
            case LM_GGUF_TYPE_INT32:   lm_gguf_set_val_i32 (ctx, src->kv[i].key.data, src->kv[i].value.int32);    break;
            case LM_GGUF_TYPE_FLOAT32: lm_gguf_set_val_f32 (ctx, src->kv[i].key.data, src->kv[i].value.float32);  break;
            case LM_GGUF_TYPE_UINT64:  lm_gguf_set_val_u64 (ctx, src->kv[i].key.data, src->kv[i].value.uint64);   break;
            case LM_GGUF_TYPE_INT64:   lm_gguf_set_val_i64 (ctx, src->kv[i].key.data, src->kv[i].value.int64);    break;
            case LM_GGUF_TYPE_FLOAT64: lm_gguf_set_val_f64 (ctx, src->kv[i].key.data, src->kv[i].value.float64);  break;
            case LM_GGUF_TYPE_BOOL:    lm_gguf_set_val_bool(ctx, src->kv[i].key.data, src->kv[i].value.bool_);    break;
            case LM_GGUF_TYPE_STRING:  lm_gguf_set_val_str (ctx, src->kv[i].key.data, src->kv[i].value.str.data); break;
            case LM_GGUF_TYPE_ARRAY:
                {
                    if (src->kv[i].value.arr.type == LM_GGUF_TYPE_STRING) {
                        const char ** data = malloc(src->kv[i].value.arr.n*sizeof(char *));
                        for (uint32_t j = 0; j < src->kv[i].value.arr.n; j++) {
                            data[j] = ((struct lm_gguf_str *)src->kv[i].value.arr.data)[j].data;
                        }
                        lm_gguf_set_arr_str(ctx, src->kv[i].key.data, data, src->kv[i].value.arr.n);
                        free(data);
                    } else if (src->kv[i].value.arr.type == LM_GGUF_TYPE_ARRAY) {
                        LM_GGML_ASSERT(false && "nested arrays not supported");
                    } else {
                        lm_gguf_set_arr_data(ctx, src->kv[i].key.data, src->kv[i].value.arr.type, src->kv[i].value.arr.data, src->kv[i].value.arr.n);
                    }
                } break;
            case LM_GGUF_TYPE_COUNT:  LM_GGML_ASSERT(false && "invalid type"); break;
        }
    }
}

void lm_gguf_add_tensor(
             struct lm_gguf_context * ctx,
        const struct lm_ggml_tensor * tensor) {
    const int idx = ctx->header.n_tensors;
    ctx->infos = realloc(ctx->infos, (idx + 1)*sizeof(struct lm_gguf_tensor_info));

    ctx->infos[idx].name.n    = strlen(tensor->name);
    ctx->infos[idx].name.data = strdup(tensor->name);

    for (int i = 0; i < LM_GGML_MAX_DIMS; ++i) {
        ctx->infos[idx].ne[i] = 1;
    }

    ctx->infos[idx].n_dims = tensor->n_dims;
    for (int i = 0; i < tensor->n_dims; i++) {
        ctx->infos[idx].ne[i] = tensor->ne[i];
    }

    ctx->infos[idx].type   = tensor->type;
    ctx->infos[idx].offset = 0;
    ctx->infos[idx].data   = tensor->data;
    ctx->infos[idx].size   = lm_ggml_nbytes(tensor);

    if (ctx->header.n_tensors > 0) {
        ctx->infos[idx].offset = ctx->infos[idx - 1].offset + LM_GGML_PAD(ctx->infos[idx - 1].size, ctx->alignment);
    }

    ctx->header.n_tensors++;
}

void lm_gguf_set_tensor_type(struct lm_gguf_context * ctx, const char * name, enum lm_ggml_type type) {
    const int idx = lm_gguf_find_tensor(ctx, name);
    if (idx < 0) {
        LM_GGML_ASSERT(false && "tensor not found");
    }

    ctx->infos[idx].type = type;
}

void lm_gguf_set_tensor_data(struct lm_gguf_context * ctx, const char * name, const void * data, size_t size) {
    const int idx = lm_gguf_find_tensor(ctx, name);
    if (idx < 0) {
        LM_GGML_ASSERT(false && "tensor not found");
    }

    ctx->infos[idx].data = data;
    ctx->infos[idx].size = size;

    // update offsets
    for (uint32_t i = idx + 1; i < ctx->header.n_tensors; ++i) {
        ctx->infos[i].offset = ctx->infos[i - 1].offset + LM_GGML_PAD(ctx->infos[i - 1].size, ctx->alignment);
    }
}

//static void lm_gguf_fwrite_str(FILE * file, const struct lm_gguf_str * val) {
//    fwrite(&val->n,   sizeof(val->n),    1, file);
//    fwrite(val->data, sizeof(char), val->n, file);
//}
//
//static void lm_gguf_fwrite_el(FILE * file, const void * val, size_t size) {
//    fwrite(val, sizeof(char), size, file);
//}

struct lm_gguf_buf {
    void * data;
    size_t size;
    size_t offset;
};

static struct lm_gguf_buf lm_gguf_buf_init(size_t size) {
    struct lm_gguf_buf buf = {
        /*buf.data   =*/ size == 0 ? NULL : malloc(size),
        /*buf.size   =*/ size,
        /*buf.offset =*/ 0,
    };

    return buf;
}

static void lm_gguf_buf_free(struct lm_gguf_buf buf) {
    if (buf.data) {
        free(buf.data);
    }
}

static void lm_gguf_buf_grow(struct lm_gguf_buf * buf, size_t size) {
    if (buf->offset + size > buf->size) {
        buf->size = 1.5*(buf->offset + size);
        if (buf->data) {
            buf->data = realloc(buf->data, buf->size);
        }
    }
}

static void lm_gguf_bwrite_str(struct lm_gguf_buf * buf, const struct lm_gguf_str * val) {
    lm_gguf_buf_grow(buf, sizeof(val->n) + val->n);

    if (buf->data) {
        memcpy((char *) buf->data + buf->offset, &val->n, sizeof(val->n));
    }
    buf->offset += sizeof(val->n);

    if (buf->data) {
        memcpy((char *) buf->data + buf->offset, val->data, val->n);
    }
    buf->offset += val->n;
}

static void lm_gguf_bwrite_el(struct lm_gguf_buf * buf, const void * val, size_t el_size) {
    lm_gguf_buf_grow(buf, el_size);

    if (buf->data) {
        memcpy((char *) buf->data + buf->offset, val, el_size);
    }
    buf->offset += el_size;
}

static void lm_gguf_write_to_buf(const struct lm_gguf_context * ctx, struct lm_gguf_buf * buf, bool only_meta) {
    // write header
    lm_gguf_bwrite_el(buf, &ctx->header.magic,     sizeof(ctx->header.magic));
    lm_gguf_bwrite_el(buf, &ctx->header.version,   sizeof(ctx->header.version));
    lm_gguf_bwrite_el(buf, &ctx->header.n_tensors, sizeof(ctx->header.n_tensors));
    lm_gguf_bwrite_el(buf, &ctx->header.n_kv,      sizeof(ctx->header.n_kv));

    // write key-value pairs
    for (uint32_t i = 0; i < ctx->header.n_kv; ++i) {
        struct lm_gguf_kv * kv = &ctx->kv[i];

        lm_gguf_bwrite_str(buf, &kv->key);
        lm_gguf_bwrite_el (buf, &kv->type, sizeof(kv->type));

        switch (kv->type) {
            case LM_GGUF_TYPE_UINT8:   lm_gguf_bwrite_el( buf, &kv->value.uint8,   sizeof(kv->value.uint8)  ); break;
            case LM_GGUF_TYPE_INT8:    lm_gguf_bwrite_el (buf, &kv->value.int8,    sizeof(kv->value.int8)   ); break;
            case LM_GGUF_TYPE_UINT16:  lm_gguf_bwrite_el (buf, &kv->value.uint16,  sizeof(kv->value.uint16) ); break;
            case LM_GGUF_TYPE_INT16:   lm_gguf_bwrite_el (buf, &kv->value.int16,   sizeof(kv->value.int16)  ); break;
            case LM_GGUF_TYPE_UINT32:  lm_gguf_bwrite_el (buf, &kv->value.uint32,  sizeof(kv->value.uint32) ); break;
            case LM_GGUF_TYPE_INT32:   lm_gguf_bwrite_el (buf, &kv->value.int32,   sizeof(kv->value.int32)  ); break;
            case LM_GGUF_TYPE_FLOAT32: lm_gguf_bwrite_el (buf, &kv->value.float32, sizeof(kv->value.float32)); break;
            case LM_GGUF_TYPE_UINT64:  lm_gguf_bwrite_el (buf, &kv->value.uint64,  sizeof(kv->value.uint64) ); break;
            case LM_GGUF_TYPE_INT64:   lm_gguf_bwrite_el (buf, &kv->value.int64,   sizeof(kv->value.int64)  ); break;
            case LM_GGUF_TYPE_FLOAT64: lm_gguf_bwrite_el (buf, &kv->value.float64, sizeof(kv->value.float64)); break;
            case LM_GGUF_TYPE_BOOL:    lm_gguf_bwrite_el (buf, &kv->value.bool_,   sizeof(kv->value.bool_)  ); break;
            case LM_GGUF_TYPE_STRING:  lm_gguf_bwrite_str(buf, &kv->value.str                               ); break;
            case LM_GGUF_TYPE_ARRAY:
                {
                    lm_gguf_bwrite_el(buf, &kv->value.arr.type, sizeof(kv->value.arr.type));
                    lm_gguf_bwrite_el(buf, &kv->value.arr.n,    sizeof(kv->value.arr.n)   );

                    switch (kv->value.arr.type) {
                        case LM_GGUF_TYPE_UINT8:
                        case LM_GGUF_TYPE_INT8:
                        case LM_GGUF_TYPE_UINT16:
                        case LM_GGUF_TYPE_INT16:
                        case LM_GGUF_TYPE_UINT32:
                        case LM_GGUF_TYPE_INT32:
                        case LM_GGUF_TYPE_FLOAT32:
                        case LM_GGUF_TYPE_UINT64:
                        case LM_GGUF_TYPE_INT64:
                        case LM_GGUF_TYPE_FLOAT64:
                        case LM_GGUF_TYPE_BOOL:
                            {
                                lm_gguf_bwrite_el(buf, kv->value.arr.data, kv->value.arr.n * LM_GGUF_TYPE_SIZE[kv->value.arr.type]);
                            } break;
                        case LM_GGUF_TYPE_STRING:
                            {
                                for (uint32_t j = 0; j < kv->value.arr.n; ++j) {
                                    lm_gguf_bwrite_str(buf, &((struct lm_gguf_str *) kv->value.arr.data)[j]);
                                }
                            } break;
                        case LM_GGUF_TYPE_ARRAY:
                        case LM_GGUF_TYPE_COUNT: LM_GGML_ASSERT(false && "invalid type"); break;
                    }
                } break;
            case LM_GGUF_TYPE_COUNT: LM_GGML_ASSERT(false && "invalid type");
        }
    }

    // write tensor infos
    for (uint32_t i = 0; i < ctx->header.n_tensors; ++i) {
        struct lm_gguf_tensor_info * info = &ctx->infos[i];

        lm_gguf_bwrite_str(buf, &info->name);
        lm_gguf_bwrite_el (buf, &info->n_dims, sizeof(info->n_dims));
        for (uint32_t j = 0; j < info->n_dims; ++j) {
            lm_gguf_bwrite_el(buf, &info->ne[j], sizeof(info->ne[j]));
        }
        lm_gguf_bwrite_el(buf, &info->type,   sizeof(info->type));
        lm_gguf_bwrite_el(buf, &info->offset, sizeof(info->offset));
    }

    // we require the data section to be aligned, so take into account any padding
    {
        const size_t offset     = buf->offset;
        const size_t offset_pad = LM_GGML_PAD(offset, ctx->alignment);

        if (offset_pad != offset) {
            uint8_t pad = 0;
            for (size_t i = 0; i < offset_pad - offset; ++i) {
                lm_gguf_bwrite_el(buf, &pad, sizeof(pad));
            }
        }
    }

    if (only_meta) {
        return;
    }

    size_t offset = 0;

    // write tensor data
    for (uint32_t i = 0; i < ctx->header.n_tensors; ++i) {
        struct lm_gguf_tensor_info * info = &ctx->infos[i];

        const size_t size     = info->size;
        const size_t size_pad = LM_GGML_PAD(size, ctx->alignment);

        lm_gguf_bwrite_el(buf, info->data, size);

        if (size_pad != size) {
            uint8_t pad = 0;
            for (size_t j = 0; j < size_pad - size; ++j) {
                lm_gguf_bwrite_el(buf, &pad, sizeof(pad));
            }
        }

        LM_GGML_ASSERT(offset == info->offset);

        offset += size_pad;
    }
}

void lm_gguf_write_to_file(const struct lm_gguf_context * ctx, const char * fname, bool only_meta) {
    FILE * file = fopen(fname, "wb");
    if (!file) {
        LM_GGML_ASSERT(false && "failed to open file for writing");
    }

    struct lm_gguf_buf buf = lm_gguf_buf_init(16*1024);

    lm_gguf_write_to_buf(ctx, &buf, only_meta);

    fwrite(buf.data, 1, buf.offset, file);

    lm_gguf_buf_free(buf);

    fclose(file);
}

size_t lm_gguf_get_meta_size(const struct lm_gguf_context * ctx) {
    // no allocs - only compute size
    struct lm_gguf_buf buf = lm_gguf_buf_init(0);

    lm_gguf_write_to_buf(ctx, &buf, true);

    return buf.offset;
}

void lm_gguf_get_meta_data(const struct lm_gguf_context * ctx, void * data) {
    struct lm_gguf_buf buf = lm_gguf_buf_init(16*1024);

    lm_gguf_write_to_buf(ctx, &buf, true);

    memcpy(data, buf.data, buf.offset);

    lm_gguf_buf_free(buf);
}

////////////////////////////////////////////////////////////////////////////////

int lm_ggml_cpu_has_avx(void) {
#if defined(__AVX__)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_avx2(void) {
#if defined(__AVX2__)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_avx512(void) {
#if defined(__AVX512F__)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_avx512_vbmi(void) {
#if defined(__AVX512VBMI__)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_avx512_vnni(void) {
#if defined(__AVX512VNNI__)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_fma(void) {
#if defined(__FMA__)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_neon(void) {
#if defined(__ARM_NEON)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_arm_fma(void) {
#if defined(__ARM_FEATURE_FMA)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_metal(void) {
#if defined(LM_GGML_USE_METAL)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_f16c(void) {
#if defined(__F16C__)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_fp16_va(void) {
#if defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_wasm_simd(void) {
#if defined(__wasm_simd128__)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_blas(void) {
#if defined(LM_GGML_USE_ACCELERATE) || defined(LM_GGML_USE_OPENBLAS) || defined(LM_GGML_USE_CUBLAS) || defined(LM_GGML_USE_CLBLAST)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_cublas(void) {
#if defined(LM_GGML_USE_CUBLAS)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_clblast(void) {
#if defined(LM_GGML_USE_CLBLAST)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_gpublas(void) {
    return lm_ggml_cpu_has_cublas() || lm_ggml_cpu_has_clblast();
}

int lm_ggml_cpu_has_sse3(void) {
#if defined(__SSE3__)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_ssse3(void) {
#if defined(__SSSE3__)
    return 1;
#else
    return 0;
#endif
}

int lm_ggml_cpu_has_vsx(void) {
#if defined(__POWER9_VECTOR__)
    return 1;
#else
    return 0;
#endif
}

////////////////////////////////////////////////////////////////////////////////
