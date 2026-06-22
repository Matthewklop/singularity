/* ============================================================================
 * run_weave_fast.c — Oracle Weave Runner (fixed sequence size)
 *
 * Quick fix version with proper sequence length for training and testing.
 * ============================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <x86intrin.h>

// ─── External functions from predict_branchless.c ───
void rule_add(uint64_t hash, uint16_t next);
int rule_find(uint64_t hash, uint16_t *next_out, float *conf_out, int min_count, float len_weight);
int predict_branchless(const uint16_t *seq, int seq_len,
                       uint16_t *prediction_out, float *confidence_out, int min_count);

// ─── Hash ───
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

#define MAX_SEQ 160000
#define N_ORBITS 4

static const uint8_t orbits[N_ORBITS][16] = {
    {3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3},
    {2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5},
    {1,1,2,3,5,8,13,5,2,7,9,0,9,9,8,1},
    {1,0,1,0,1,0,1,0,2,1,2,1,2,1,2,1},
};

// ─── Scalar predictor (for comparison) ───
typedef struct { uint64_t ngram_hash; uint16_t next; uint16_t count; uint16_t conf; } srule_t;
#define MAX_SR 2048
static srule_t sr[MAX_SR];
static int nsr = 0;

static void s_add(uint64_t h, uint16_t n) {
    for (int i = 0; i < nsr; i++)
        if (sr[i].ngram_hash == h && sr[i].next == n) { sr[i].count++; sr[i].conf = sr[i].count * 65535 / 255; return; }
    if (nsr < MAX_SR) { int r = nsr++; sr[r].ngram_hash = h; sr[r].next = n; sr[r].count = 1; sr[r].conf = 257; }
}

static int s_pred(const uint16_t *seq, int sl, uint16_t *po, float *co, int mc) {
    if (sl < 2 || nsr < 1) { *co = 0; return 0; }
    uint16_t bp = 0; float bc = 0;
    for (int l = sl; l >= 2; l--) {
        uint64_t h = hash_ngram(seq + sl - l, l);
        float w = (float)l / sl;
        for (int r = 0; r < nsr; r++)
            if (sr[r].ngram_hash == h && sr[r].count >= mc) {
                float c = (float)sr[r].conf / 65535.0f * w;
                if (c > bc) { bc = c; bp = sr[r].next; }
            }
        if (bc > 0.3f) break;
    }
    *po = bp; *co = bc;
    return bc > 0.05f;
}

int main() {
    printf("╔══════════════════════════════════════╗\n");
    printf("║   ORACLE WEAVE — RUNNER              ║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    // Generate sequence
    uint16_t *seq = malloc(MAX_SEQ * sizeof(uint16_t));
    int sl = 0;
    for (int c = 0; c < 40000 && sl < MAX_SEQ; c++)
        for (int o = 0; o < N_ORBITS && sl < MAX_SEQ; o++)
            seq[sl++] = orbits[o][c % 16];
    printf("  Sequence: %d entries\n", sl);

    // Train both engines
    printf("  Training...\n");
    uint64_t t0 = rdtscp();
    int trained = 0;
    for (int i = 16; i < sl; i++)
        for (int n = 2; n <= 16 && n <= i; n++) {
            uint64_t h = hash_ngram(seq + i - n, n);
            s_add(h, seq[i]);
            rule_add(h, seq[i]);
            trained++;
        }
    uint64_t t1 = rdtscp();
    printf("  %d transitions in %.0f cycles\n", trained, (float)(t1 - t0));

    // Benchmark: SIMD
    printf("\n── SIMD branchless prediction ──\n");
    int bench_start = 40000 * 4;
    if (bench_start + 16 >= sl) bench_start = sl - 8000 - 16;
    int te = bench_start + 8000;
    if (te > sl) te = sl;
    printf("  Testing from %d to %d\n", bench_start, te);

    int sc = 0, st = 0;
    t0 = rdtscp();
    for (int i = bench_start + 16; i < te; i++) {
        uint16_t p; float c;
        if (predict_branchless(seq + i - 16, 16, &p, &c, 3)) {
            st++;
            if (p == seq[i]) sc++;
        }
    }
    t1 = rdtscp();
    float acc = st > 0 ? (float)sc / st : 0;
    float cpp = st > 0 ? (float)(t1 - t0) / st : 0;
    printf("  %d/%d correct (%.1f%%)\n", sc, st, acc * 100);
    printf("  %.0f cycles/prediction\n", cpp);

    // Benchmark: Scalar
    printf("\n── Scalar prediction ──\n");
    int sc2 = 0, st2 = 0;
    uint64_t t2 = rdtscp();
    for (int i = bench_start + 16; i < te; i++) {
        uint16_t p; float c;
        if (s_pred(seq + i - 16, 16, &p, &c, 3)) {
            st2++;
            if (p == seq[i]) sc2++;
        }
    }
    uint64_t t3 = rdtscp();
    float acc2 = st2 > 0 ? (float)sc2 / st2 : 0;
    float cpp2 = st2 > 0 ? (float)(t3 - t2) / st2 : 0;
    printf("  %d/%d correct (%.1f%%)\n", sc2, st2, acc2 * 100);
    printf("  %.0f cycles/prediction\n", cpp2);

    // Comparison
    if (st > 0 && st2 > 0) {
        printf("\n── Comparison ──\n");
        printf("  SIMD:   %.0f cpp (%.1f%%)\n", cpp, acc * 100);
        printf("  Scalar: %.0f cpp (%.1f%%)\n", cpp2, acc2 * 100);
        printf("  Speedup: %.2fx\n", cpp2 / cpp);
    }

    // Live machine state
    printf("\n── Live machine state ──\n");
    FILE *fp;
    char buf[256];
    float state[4] = {0};

    fp = fopen("/proc/stat", "r");
    if (fp && fgets(buf, sizeof(buf), fp)) {
        unsigned long u, n, s, i;
        sscanf(buf, "cpu %lu %lu %lu %lu", &u, &n, &s, &i);
        unsigned long tot = u + n + s + i;
        state[0] = tot > 0 ? (float)(u + s) / tot : 0;
        fclose(fp);
    }
    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        unsigned long mt = 1, ma = 0;
        while (fgets(buf, sizeof(buf), fp)) {
            sscanf(buf, "MemTotal: %lu", &mt);
            if (sscanf(buf, "MemAvailable: %lu", &ma)) break;
        }
        state[1] = mt > 0 ? 1.0f - (float)ma / mt : 0;
        fclose(fp);
    }
    fp = fopen("/proc/loadavg", "r");
    if (fp && fgets(buf, sizeof(buf), fp)) {
        double l1; unsigned long r, t;
        sscanf(buf, "%lf %*f %*f %lu/%lu", &l1, &r, &t);
        state[2] = (float)(l1 / 16.0);
        state[3] = t > 0 ? (float)r / t : 0;
        fclose(fp);
    }

    uint16_t ls[4];
    printf("  CPU: %.1f%%  Mem: %.1f%%  Load: %.2f  Run: %.0f\n",
           state[0]*100, state[1]*100, state[2], state[3]*100);
    for (int i = 0; i < 4; i++) ls[i] = (uint16_t)(state[i] * 65535) % 16;
    printf("  Attractor prefix: %d %d %d %d\n", ls[0], ls[1], ls[2], ls[3]);

    uint16_t pp; float pc;
    if (predict_branchless(ls, 4, &pp, &pc, 1))
        printf("  SIMD predicts next: %d (%.0f%%)\n", pp, pc * 100);
    else
        printf("  SIMD: no prediction for live state\n");

    printf("\n╔══════════════════════════════════════╗\n");
    printf("║   WEAVE COMPLETE                     ║\n");
    printf("╚══════════════════════════════════════╝\n");

    free(seq);
    return 0;
}
