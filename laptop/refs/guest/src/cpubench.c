// cpubench.c — a small, fixed-work CPU/memory benchmark to compare native
// x86-64 (the laptop, inside the rootfs) with FEX on the Apple TV. Each test
// does a fixed amount of work and prints ns per operation; the ratio TV/laptop
// per test is the emulation slowdown for that kind of code. Roughly one
// second per test natively.
//
//   gcc -O2 -o cpubench cpubench.c -lm      (rootfs-customize.sh does this)
//   ./cpubench [scale]                       scale < 1 shortens every test
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static volatile uint64_t sink;

// 1. integer ALU: xorshift + multiply mix (branch-free)
static double t_int_alu(uint64_t iters) {
    uint64_t x = 0x9E3779B97F4A7C15ull, acc = 0;
    double t0 = now();
    for (uint64_t i = 0; i < iters; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        acc += (x * 0xD1B54A32D192ED03ull) >> 29;
    }
    double dt = now() - t0;
    sink = acc;
    return dt * 1e9 / (double)iters;
}

// 2. scalar floating point: a 4-body spring/gravity integrator with sqrt
static double t_fp_scalar(uint64_t steps) {
    double px[4] = {0, 1, 2, 3}, py[4] = {1, 0, -1, 0.5}, vx[4] = {0}, vy[4] = {0};
    double t0 = now();
    for (uint64_t s = 0; s < steps; s++) {
        for (int i = 0; i < 4; i++) {
            double ax = 0, ay = 0;
            for (int j = 0; j < 4; j++) {
                if (i == j) continue;
                double dx = px[j] - px[i], dy = py[j] - py[i];
                double r2 = dx * dx + dy * dy + 0.01;
                double inv = 1.0 / (r2 * sqrt(r2));
                ax += dx * inv;
                ay += dy * inv;
            }
            vx[i] += ax * 1e-4;
            vy[i] += ay * 1e-4;
        }
        for (int i = 0; i < 4; i++) {
            px[i] += vx[i] * 1e-3;
            py[i] += vy[i] * 1e-3;
        }
    }
    double dt = now() - t0;
    sink = (uint64_t)(px[0] * 1e6);
    return dt * 1e9 / (double)steps;
}

// 3. SIMD-friendly float loops (gcc -O2 vectorizes with SSE2): FEX's vector path
static double t_fp_simd(uint64_t reps) {
    enum { N = 4096 };
    static float a[N], b[N], c[N];
    for (int i = 0; i < N; i++) {
        a[i] = (float)i * 0.5f;
        b[i] = (float)(N - i) * 0.25f;
        c[i] = 1.0f;
    }
    double t0 = now();
    for (uint64_t r = 0; r < reps; r++) {
        for (int i = 0; i < N; i++) c[i] = c[i] * 0.999f + a[i] * b[i] * 0.001f;
    }
    double dt = now() - t0;
    float s = 0;
    for (int i = 0; i < N; i++) s += c[i];
    sink = (uint64_t)s;
    return dt * 1e9 / ((double)reps * N);  // ns per element
}

// 4. memcpy bandwidth (16 MB buffers)
static double t_memcpy(uint64_t reps, double* gbs) {
    size_t n = 16u << 20;
    char* src = malloc(n);
    char* dst = malloc(n);
    memset(src, 1, n);
    memset(dst, 0, n);
    double t0 = now();
    for (uint64_t r = 0; r < reps; r++) {
        memcpy(dst, src, n);
        src[r % n]++;
    }
    double dt = now() - t0;
    sink = (uint64_t)dst[12345];
    *gbs = (double)n * (double)reps / dt / 1e9;
    free(src);
    free(dst);
    return dt * 1e9 / (double)reps;
}

// 5. random 8-byte reads over 64 MB (cache-miss bound)
static double t_mem_random(uint64_t reads) {
    size_t n = (64u << 20) / 8;
    uint64_t* arr = malloc(n * 8);
    for (size_t i = 0; i < n; i++) arr[i] = (i * 0x9E3779B97F4A7C15ull) % n;
    uint64_t idx = 1, acc = 0;
    double t0 = now();
    for (uint64_t i = 0; i < reads; i++) {
        idx = arr[idx];
        acc += idx;
    }
    double dt = now() - t0;
    sink = acc;
    free(arr);
    return dt * 1e9 / (double)reads;
}

// 6. branchy code: repeated insertion sort of 2K ints (data-dependent branches)
static double t_branchy(uint64_t reps) {
    enum { N = 2048 };
    static int v[N];
    double t0 = now();
    uint64_t acc = 0;
    for (uint64_t r = 0; r < reps; r++) {
        uint32_t x = (uint32_t)r * 2654435761u;
        for (int i = 0; i < N; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            v[i] = (int)(x & 0xFFFF);
        }
        for (int i = 1; i < N; i++) {
            int key = v[i], j = i - 1;
            while (j >= 0 && v[j] > key) {
                v[j + 1] = v[j];
                j--;
            }
            v[j + 1] = key;
        }
        acc += (uint64_t)v[N / 2];
    }
    double dt = now() - t0;
    sink = acc;
    return dt * 1e9 / ((double)reps * N * N / 4);  // ns per inner step (average)
}

// 7. calls: recursion + indirect calls (FEX's call/ret prediction)
typedef uint64_t (*fn_t)(uint64_t);
static uint64_t f_add(uint64_t x) { return x + 1; }
static uint64_t f_mul(uint64_t x) { return x * 3; }
static uint64_t f_xor(uint64_t x) { return x ^ 0x55; }
static uint64_t f_shr(uint64_t x) { return x >> 1; }
static uint64_t fib(uint64_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static double t_calls(uint64_t reps) {
    static const fn_t table[4] = {f_add, f_mul, f_xor, f_shr};
    double t0 = now();
    uint64_t acc = 0;
    for (uint64_t r = 0; r < reps; r++) {
        acc += fib(20);
        uint64_t x = r;
        for (int i = 0; i < 64; i++) x = table[x & 3](x);
        acc += x;
    }
    double dt = now() - t0;
    sink = acc;
    return dt * 1e9 / ((double)reps * (21891 + 64));  // ns per call (fib(20) = 21891 calls)
}

int main(int argc, char** argv) {
    double scale = argc > 1 ? atof(argv[1]) : 1.0;
    if (scale <= 0) scale = 1.0;
    double T0 = now();
    double gbs = 0;
    double r_int = t_int_alu((uint64_t)(400e6 * scale));
    double r_fp = t_fp_scalar((uint64_t)(4e6 * scale));
    double r_simd = t_fp_simd((uint64_t)(60000 * scale));
    double r_cpy = t_memcpy((uint64_t)(160 * scale), &gbs);
    double r_rnd = t_mem_random((uint64_t)(10e6 * scale));
    double r_br = t_branchy((uint64_t)(400 * scale));
    double r_call = t_calls((uint64_t)(4000 * scale));
    double total = now() - T0;
    printf("cpubench: int_alu %.2f ns/op | fp_scalar %.1f ns/step | simd %.3f ns/elem | memcpy %.2f GB/s (%.1f ms/16MB) | "
           "mem_random %.1f ns/read | branchy %.2f ns/step | calls %.2f ns/call | total %.1f s\n",
           r_int, r_fp, r_simd, gbs, r_cpy / 1e6, r_rnd, r_br, r_call, total);
    printf("{\"int_alu_ns\":%.3f,\"fp_scalar_ns\":%.2f,\"simd_ns\":%.4f,\"memcpy_gbs\":%.3f,\"mem_random_ns\":%.2f,"
           "\"branchy_ns\":%.3f,\"calls_ns\":%.3f,\"total_s\":%.2f,\"scale\":%.3f}\n",
           r_int, r_fp, r_simd, gbs, r_rnd, r_br, r_call, total, scale);
    return 0;
}
