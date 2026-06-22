/* ============================================================================
 * run_weave.c — Oracle Weave Runner
 *
 * Runs the singularity kernel with the AVX2 branchless prediction engine.
 * Compares scalar vs SIMD prediction performance.
 *
 * Build: gcc -O3 -mavx2 -march=native -lpthread -lm \
 *        run_weave.c singularity.c predict_branchless.c \
 *        -o run_weave
 *
 * Run:   ./run_weave [--seconds N] [--simd] [--scalar]
 * ============================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <x86intrin.h>

// ─── Include the branchless prediction engine ───
// predict_branchless.c provides:
//   - rule_add(hash, next)
//   - rule_find(hash, &next, &conf, min_count, len_weight)
//   - predict_branchless(seq, len, &pred, &conf, min_count)

// ─── Hash helpers (replicated for standalone build) ───
static inline uint64_t hash_ngram(const uint16_t *seq, int n) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (int i = 0; i < n; i++) { h ^= seq[i]; h *= 0x100000001B3ULL; }
    return h;
}

static inline uint64_t fast_hash(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static inline uint64_t rdtscp(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "ecx");
    return ((uint64_t)hi << 32) | lo;
}

// ─── Weave state ───
#define STATE_DIM 16
#define MAX_ATTRACTORS 64
#define MAX_SEQ 1024
#define N_ORBITS 4

// Deterministic orbits
static const uint8_t orbits[N_ORBITS][16] = {
    {3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3},
    {2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5},
    {1,1,2,3,5,8,13,5,2,7,9,0,9,9,8,1},
    {1,0,1,0,1,0,1,0,2,1,2,1,2,1,2,1},
};

// ─── Scalar (original) prediction ───
// Simulates the original singularity kernel's predict_next logic
typedef struct {
    uint64_t ngram_hash;
    uint16_t next_attractor;
    uint16_t count;
    uint16_t confidence;
} scalar_rule_t;

#define MAX_SCALAR_RULES 2048
static scalar_rule_t scalar_rules[MAX_SCALAR_RULES];
static int n_scalar_rules = 0;

static void scalar_add(uint64_t hash, uint16_t next) {
    for (int i = 0; i < n_scalar_rules; i++) {
        if (scalar_rules[i].ngram_hash == hash && scalar_rules[i].next_attractor == next) {
            if (scalar_rules[i].count < 65535) scalar_rules[i].count++;
            scalar_rules[i].confidence = scalar_rules[i].count * 65535 / 255;
            if (scalar_rules[i].confidence > 65535) scalar_rules[i].confidence = 65535;
            return;
        }
    }
    if (n_scalar_rules < MAX_SCALAR_RULES) {
        int r = n_scalar_rules++;
        scalar_rules[r].ngram_hash = hash;
        scalar_rules[r].next_attractor = next;
        scalar_rules[r].count = 1;
        scalar_rules[r].confidence = 257;
    }
}

static int scalar_predict(const uint16_t *seq, int seq_len,
                          uint16_t *pred_out, float *conf_out, int min_count) {
    if (seq_len < 2 || n_scalar_rules < 1) { *conf_out = 0.0f; return 0; }
    if (min_count < 1) min_count = 3;
    
    uint16_t best_pred = 0;
    float best_conf = 0.0f;
    
    for (int len = seq_len; len >= 2; len--) {
        uint64_t hash = hash_ngram(seq + seq_len - len, len);
        float weight = (float)len / (float)seq_len;
        
        for (int r = 0; r < n_scalar_rules; r++) {
            if (scalar_rules[r].ngram_hash == hash && scalar_rules[r].count >= min_count) {
                float conf = (float)scalar_rules[r].confidence / 65535.0f * weight;
                if (conf > best_conf) { best_conf = conf; best_pred = scalar_rules[r].next_attractor; }
            }
        }
        if (best_conf > 0.3f) break;
    }
    
    *pred_out = best_pred;
    *conf_out = best_conf;
    return best_conf > 0.05f;
}

// ─── Weave runner ───
// Trains both engines on the same deterministic orbits,
// then benchmarks their prediction speed and accuracy.

int main(int argc, char **argv) {
    int run_seconds = 10;
    int do_simd = 1;
    int do_scalar = 1;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i+1 < argc) run_seconds = atoi(argv[++i]);
        if (strcmp(argv[i], "--simd-only") == 0) do_scalar = 0;
        if (strcmp(argv[i], "--scalar-only") == 0) do_simd = 0;
    }
    
    printf("╔══════════════════════════════════════╗\n");
    printf("║   ORACLE WEAVE — RUNNER             ║\n");
    printf("╚══════════════════════════════════════╝\n");
    printf("\n");
    printf("  Duration: %d seconds\n", run_seconds);
    printf("  SIMD engine:  %s\n", do_simd ? "ENABLED" : "disabled");
    printf("  Scalar engine: %s\n", do_scalar ? "ENABLED" : "disabled");
    printf("\n");
    
    // ─── Phase 1: Train both engines on deterministic orbits ───
    printf("── Phase 1: Training on deterministic orbits ──\n");
    
    uint16_t seq[MAX_SEQ];
    int seq_len = 0;
    
    // Generate sequence from all 4 orbits interleaved
    for (int cycle = 0; cycle < 10000; cycle++) {
        for (int o = 0; o < N_ORBITS; o++) {
            int idx = cycle % 16;
            uint16_t attr = orbits[o][idx];
            if (seq_len < MAX_SEQ) seq[seq_len++] = attr;
        }
    }
    
    printf("  Generated %d attractor sequence entries\n", seq_len);
    
    // Train on n-grams
    int n_trained = 0;
    for (int i = 16; i < seq_len; i++) {
        for (int n = 2; n <= 16 && n <= i; n++) {
            uint64_t hash = hash_ngram(seq + i - n, n);
            uint16_t next = seq[i];
            
            if (do_scalar) scalar_add(hash, next);
            if (do_simd)   rule_add(hash, next);
            n_trained++;
        }
    }
    
    printf("  Trained %d transitions\n", n_trained);
    
    // Report rule counts
    int n_simd_rules = 0;  // extracted from predict_branchless.c's global
    printf("  Scalar rules: %d\n", n_scalar_rules);
    printf("  SIMD rules:   (see predict_branchless.c internals)\n");
    printf("\n");
    
    // ─── Phase 2: Benchmark prediction throughput ───
    printf("── Phase 2: Benchmarking prediction throughput ──\n");
    
    float simd_accuracy = 0.0f;
    float scalar_accuracy = 0.0f;
    uint64_t simd_cycles = 0;
    uint64_t scalar_cycles = 0;
    int simd_correct = 0, simd_total = 0;
    int scalar_correct = 0, scalar_total = 0;
    
    // Test on sequence from cycle 10000 to 12000 (fresh data)
    int test_start = 10000 * 4;  // 10000 cycles * 4 orbits
    int test_end = test_start + 2000 * 4;
    
    if (do_simd) {
        printf("\n  SIMD branchless engine:\n");
        uint64_t t0 = rdtscp();
        
        for (int i = test_start + 16; i < test_end && i < seq_len; i++) {
            uint16_t pred; float conf;
            int found = predict_branchless(seq + i - 16, 16, &pred, &conf, 3);
            if (found) {
                simd_total++;
                if (pred == seq[i]) simd_correct++;
            }
        }
        
        simd_cycles = rdtscp() - t0;
        simd_accuracy = simd_total > 0 ? (float)simd_correct / simd_total : 0.0f;
        printf("    Predictions: %d correct / %d total (%.1f%%)\n",
               simd_correct, simd_total, simd_accuracy * 100);
        printf("    Cycles: %lu (%.0f cycles/prediction)\n",
               simd_cycles, simd_total > 0 ? (float)simd_cycles / simd_total : 0);
    }
    
    if (do_scalar) {
        printf("\n  Scalar engine:\n");
        uint64_t t0 = rdtscp();
        
        for (int i = test_start + 16; i < test_end && i < seq_len; i++) {
            uint16_t pred; float conf;
            int found = scalar_predict(seq + i - 16, 16, &pred, &conf, 3);
            if (found) {
                scalar_total++;
                if (pred == seq[i]) scalar_correct++;
            }
        }
        
        scalar_cycles = rdtscp() - t0;
        scalar_accuracy = scalar_total > 0 ? (float)scalar_correct / scalar_total : 0.0f;
        printf("    Predictions: %d correct / %d total (%.1f%%)\n",
               scalar_correct, scalar_total, scalar_accuracy * 100);
        printf("    Cycles: %lu (%.0f cycles/prediction)\n",
               scalar_cycles, scalar_total > 0 ? (float)scalar_cycles / scalar_total : 0);
    }
    
    // ─── Phase 3: Speed comparison ───
    if (do_simd && do_scalar && simd_total > 0 && scalar_total > 0) {
        printf("\n── Phase 3: Comparison ──\n");
        float simd_cpb = (float)simd_cycles / simd_total;
        float scalar_cpb = (float)scalar_cycles / scalar_total;
        float speedup = scalar_cpb / simd_cpb;
        printf("  SIMD:   %.0f cycles/prediction\n", simd_cpb);
        printf("  Scalar: %.0f cycles/prediction\n", scalar_cpb);
        printf("  Speedup: %.2fx\n", speedup);
        printf("\n");
        printf("  SIMD accuracy:   %.1f%%\n", simd_accuracy * 100);
        printf("  Scalar accuracy: %.1f%%\n", scalar_accuracy * 100);
    }
    
    // ─── Phase 4: Live machine state weave ───
    printf("\n── Phase 4: Live machine state → attractor mapping ──\n");
    printf("  Reading /proc/stat, /proc/meminfo, /proc/loadavg...\n");
    
    float state[STATE_DIM];
    FILE *fp;
    char buf[256];
    
    // CPU load
    fp = fopen("/proc/stat", "r");
    if (fp && fgets(buf, sizeof(buf), fp)) {
        unsigned long user, nice, sys, idle;
        sscanf(buf, "cpu %lu %lu %lu %lu", &user, &nice, &sys, &idle);
        unsigned long total = user + nice + sys + idle;
        state[0] = total > 0 ? (float)(user + sys) / total : 0.0f;
        fclose(fp);
    }
    
    // Memory
    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        unsigned long mem_total = 1, mem_avail = 0;
        while (fgets(buf, sizeof(buf), fp)) {
            sscanf(buf, "MemTotal: %lu", &mem_total);
            if (sscanf(buf, "MemAvailable: %lu", &mem_avail)) break;
        }
        state[1] = mem_total > 0 ? 1.0f - (float)mem_avail / mem_total : 0.0f;
        fclose(fp);
    }
    
    // Load
    fp = fopen("/proc/loadavg", "r");
    if (fp && fgets(buf, sizeof(buf), fp)) {
        double l1; unsigned long run, total;
        sscanf(buf, "%lf %*f %*f %lu/%lu", &l1, &run, &total);
        state[2] = (float)(l1 / 16.0);
        state[3] = total > 0 ? (float)run / total : 0.0f;
        fclose(fp);
    }
    
    printf("  Machine state vector:\n");
    for (int i = 0; i < 4; i++) {
        printf("    state[%d] = %.3f\n", i, state[i]);
    }
    
    // Encode state as attractor sequence for prediction
    uint16_t live_seq[4];
    for (int i = 0; i < 4; i++) {
        live_seq[i] = (uint16_t)(state[i] * 65535.0f) % 16;
    }
    
    printf("\n  Live attractor prefix: ");
    for (int i = 0; i < 4; i++) printf("%d ", live_seq[i]);
    printf("\n");
    
    // Predict next state (using whatever rules were trained)
    if (do_simd) {
        uint16_t pred; float conf;
        int found = predict_branchless(live_seq, 4, &pred, &conf, 1);
        if (found) {
            printf("  SIMD predicts next attractor: %d (confidence: %.1f%%)\n", pred, conf * 100);
        } else {
            printf("  SIMD: no prediction (insufficient rules for live state)\n");
        }
    }
    
    printf("\n╔══════════════════════════════════════╗\n");
    printf("║   WEAVE COMPLETE                     ║\n");
    printf("╚══════════════════════════════════════╝\n");
    
    return 0;
}
