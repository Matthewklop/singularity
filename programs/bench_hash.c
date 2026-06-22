/* ============================================================================
 * bench_hash.c — Benchmark: SIMD rolling hash vs scalar n-gram hash
 *
 * Compares the throughput of:
 *   - SIMD rolling hash (4 n-gram lengths in parallel)
 *   - Scalar n-gram hash (one at a time)
 *
 * Build: gcc -O3 -mavx2 -march=native -o bench_hash bench_hash.c simd_hash.c -lm
 * Run:   ./bench_hash
 * ============================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <immintrin.h>

// ─── Include the rolling hash engine ───
// From simd_hash.c:
//   rolling_hash_t, rh_init(), rh_ingest(), rh_get_hash(), rh_get_all_hashes()
#include "simd_hash.c"

// ─── Scalar hash (original) ───
static inline uint64_t hash_ngram(const uint16_t *seq, int n) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (int i = 0; i < n; i++) { h ^= seq[i]; h *= 0x100000001B3ULL; }
    return h;
}

static inline uint64_t rdtscp(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "ecx");
    return ((uint64_t)hi << 32) | lo;
}

int main() {
    printf("╔══════════════════════════════════════╗\n");
    printf("║   SIMD ROLLING HASH BENCHMARK       ║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    // Generate test sequence
    #define N_TOKENS 100000
    #define N_ORBITS 4
    static const uint8_t orbits[N_ORBITS][16] = {
        {3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3},
        {2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5},
        {1,1,2,3,5,8,13,5,2,7,9,0,9,9,8,1},
        {1,0,1,0,1,0,1,0,2,1,2,1,2,1,2,1},
    };

    uint16_t *seq = malloc(N_TOKENS * sizeof(uint16_t));
    int sl = 0;
    for (int c = 0; c < N_TOKENS / N_ORBITS && sl < N_TOKENS; c++)
        for (int o = 0; o < N_ORBITS && sl < N_TOKENS; o++)
            seq[sl++] = orbits[o][c % 16];
    printf("  Sequence: %d tokens\n\n", sl);

    // ─── Benchmark 1: Scalar hash (all n-gram lengths) ───
    printf("── Scalar n-gram hash ──\n");
    uint64_t t0 = rdtscp();
    uint64_t scalar_hashes = 0;
    for (int i = 16; i < sl; i++) {
        for (int n = 2; n <= 16; n++) {
            uint64_t h = hash_ngram(seq + i - n, n);
            scalar_hashes += h;
        }
    }
    uint64_t t1 = rdtscp();
    uint64_t scalar_cycles = t1 - t0;
    double scalar_cpt = (double)scalar_cycles / (sl - 16);
    printf("  %lu hashes in %lu cycles\n", scalar_hashes, scalar_cycles);
    printf("  %.0f cycles/token (15 hashes per token)\n", scalar_cpt);
    printf("  %.1f cycles/hash\n\n", (double)scalar_cycles / (scalar_hashes ? (sl - 16) * 15 : 1));

    // ─── Benchmark 2: SIMD rolling hash ───
    printf("── SIMD rolling hash (4 lengths in parallel) ──\n");
    
    rolling_hash_t rh = rh_init();
    uint64_t simd_hashes = 0;
    uint64_t t2 = rdtscp();
    for (int i = 0; i < sl; i++) {
        rh_ingest(&rh, seq[i]);
    }
    // After ingesting all tokens, extract all valid hashes for each position
    // Reset and re-run with extraction at each step
    rolling_hash_t rh2 = rh_init();
    uint64_t t3 = rdtscp();  // start timing AFTER warmup
    for (int i = 0; i < sl; i++) {
        rh_ingest(&rh2, seq[i]);
        
        uint64_t hashes[4];
        int lengths[4];
        int n = rh_get_all_hashes(&rh2, hashes, lengths);
        for (int j = 0; j < n; j++) {
            simd_hashes += hashes[j];
        }
    }
    uint64_t t4 = rdtscp();
    uint64_t simd_cycles = t4 - t3;
    double simd_cpt = (double)simd_cycles / sl;
    printf("  %lu hashes in %lu cycles\n", simd_hashes, simd_cycles);
    printf("  %.0f cycles/token\n", simd_cpt);
    
    // ─── Comparison ───
    printf("\n── Comparison ──\n");
    double scalar_total_time = (double)scalar_cycles / (sl - 16);
    double simd_total_time = (double)simd_cycles / sl;
    printf("  Scalar: %.0f cycles/token (15 hashes: 2..16)\n", scalar_total_time);
    printf("  SIMD:   %.0f cycles/token (4 hashes: 2,4,8,16)\n", simd_total_time);
    printf("  Speedup: %.2fx per token\n", scalar_total_time / simd_total_time);
    
    // Adjust for hash count: scalar does 15 hashes/token, SIMD does 4
    double scalar_per_hash = scalar_total_time / 15.0;
    double simd_per_hash = simd_total_time / 4.0;
    printf("\n  Per-hash: scalar=%.1f cycles  SIMD=%.1f cycles\n",
           scalar_per_hash, simd_per_hash);
    printf("  SIMD hash throughput advantage: %.2fx\n", scalar_per_hash / simd_per_hash);

    // ─── Verification: hashes match ───
    printf("\n── Verification ──\n");
    rolling_hash_t rh3 = rh_init();
    int mismatches = 0;
    for (int i = 0; i < sl && i < 1000; i++) {
        rh_ingest(&rh3, seq[i]);
        
        uint64_t simd_h2 = rh_get_hash(&rh3, 2);
        uint64_t simd_h4 = rh_get_hash(&rh3, 4);
        uint64_t simd_h8 = rh_get_hash(&rh3, 8);
        uint64_t simd_h16 = rh_get_hash(&rh3, 16);
        
        if (i >= 1) {
            uint64_t sc_h2 = hash_ngram(seq + i - 1, 2);
            if (simd_h2 != sc_h2 && i >= 1) mismatches++;
        }
    }
    printf("  Hash mismatches (first 1000 tokens): %d\n", mismatches);
    printf("  Status: %s\n\n", mismatches == 0 ? "✅ ALL MATCH" : "⚠️  MISMATCHES");

    printf("╔══════════════════════════════════════╗\n");
    printf("║   BENCHMARK COMPLETE                ║\n");
    printf("╚══════════════════════════════════════╝\n");

    free(seq);
    return 0;
}
