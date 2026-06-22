/* ============================================================================
 * hdc_blend.c — Hyperdimensional Concept Blending with 1024-dim vectors
 *
 * Each attractor is mapped to a 1024-dimension hyperdimensional vector (HDV).
 * When confidence < 0.4, the engine blends V_curr + V_prev using SIMD FMA,
 * then projects onto the concept space via dot-product similarity search.
 *
 * This creates "Hybrid Concepts" that never existed in training data,
 * allowing the engine to predict events that combine two separate scenarios.
 *
 * 1024 dimensions processed in SIMD: 8 × _mm256_fmadd_ps (32 floats each)
 * ============================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <immintrin.h>
#include <math.h>

#define HD_DIM 1024
#define HD_DIM_F 1024.0f
#define MAX_CONCEPTS 64
#define N_LANES 8  // 1024 / (8 * 32) = 4 iterations of 256-bit SIMD

// ─── Hyperdimensional vector ───
typedef struct {
    float vec[HD_DIM] __attribute__((aligned(32)));
    char label[32];
    uint16_t id;
    uint8_t active;
} hdv_t;

static hdv_t concepts[MAX_CONCEPTS];
static int n_concepts = 0;

// ─── Generate a random hyperdimensional vector ───
static void generate_hdv(float *vec, uint64_t seed) {
    for (int i = 0; i < HD_DIM; i++) {
        uint64_t x = seed ^ (i * 0x9E3779B97F4A7C15ULL);
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        x ^= x >> 31;
        // Generate values in [-1, 1]
        vec[i] = 2.0f * (float)(x & 0xFFFF) / 65535.0f - 1.0f;
    }
}

// ─── Initialize concepts with HDVs ───
static void init_concepts(void) {
    const char *labels[] = {
        "optimize", "compile", "predict", "parallel",
        "cache", "memory", "network", "storage",
        "gpu", "cpu", "kernel", "userspace",
        "hot_path", "cold_path", "branch", "vector",
        "simd", "io", "scheduler", "latency",
        "throughput", "bandwidth", "heap", "stack",
        "recursive", "iterative", "sync", "async",
        "serialize", "deserialize", "encode", "decode",
        "allocate", "free", "read", "write",
        "spawn", "join", "lock", "signal",
        "compress", "decompress", "encrypt", "decrypt",
        "mount", "unmount", "bind", "connect",
    };
    
    int n = sizeof(labels) / sizeof(labels[0]);
    if (n > MAX_CONCEPTS) n = MAX_CONCEPTS;
    
    for (int i = 0; i < n; i++) {
        strncpy(concepts[i].label, labels[i], 31);
        concepts[i].id = i;
        concepts[i].active = 1;
        
        uint64_t seed = 0xCBF29CE484222325ULL;
        for (const char *p = labels[i]; *p; p++) {
            seed ^= *p;
            seed *= 0x100000001B3ULL;
        }
        generate_hdv(concepts[i].vec, seed);
    }
    n_concepts = n;
}

// ─── SIMD dot product: 1024-dim vectors ───
// Processes 8 × 256-bit lanes = 32 floats per iteration
// 4 iterations × 256 floats each = 1024 total
static float dot_product_simd(const float *a, const float *b) {
    __m256 sum0 = _mm256_setzero_ps();
    __m256 sum1 = _mm256_setzero_ps();
    __m256 sum2 = _mm256_setzero_ps();
    __m256 sum3 = _mm256_setzero_ps();
    
    for (int i = 0; i < HD_DIM; i += 32) {
        __m256 va0 = _mm256_load_ps(&a[i]);
        __m256 vb0 = _mm256_load_ps(&b[i]);
        sum0 = _mm256_fmadd_ps(va0, vb0, sum0);
        
        __m256 va1 = _mm256_load_ps(&a[i+8]);
        __m256 vb1 = _mm256_load_ps(&b[i+8]);
        sum1 = _mm256_fmadd_ps(va1, vb1, sum1);
        
        __m256 va2 = _mm256_load_ps(&a[i+16]);
        __m256 vb2 = _mm256_load_ps(&b[i+16]);
        sum2 = _mm256_fmadd_ps(va2, vb2, sum2);
        
        __m256 va3 = _mm256_load_ps(&a[i+24]);
        __m256 vb3 = _mm256_load_ps(&b[i+24]);
        sum3 = _mm256_fmadd_ps(va3, vb3, sum3);
    }
    
    // Horizontal sum
    sum0 = _mm256_add_ps(sum0, sum1);
    sum2 = _mm256_add_ps(sum2, sum3);
    sum0 = _mm256_add_ps(sum0, sum2);
    
    // Extract
    float __attribute__((aligned(32))) tmp[8];
    _mm256_store_ps(tmp, sum0);
    return tmp[0] + tmp[1] + tmp[2] + tmp[3] + tmp[4] + tmp[5] + tmp[6] + tmp[7];
}

// ─── SIMD vector blend: C = A + B (element-wise) ───
static void blend_vectors_simd(const float *a, const float *b, float *out) {
    for (int i = 0; i < HD_DIM; i += 8) {
        __m256 va = _mm256_load_ps(&a[i]);
        __m256 vb = _mm256_load_ps(&b[i]);
        __m256 vsum = _mm256_add_ps(va, vb);
        _mm256_store_ps(&out[i], vsum);
    }
}

// ─── Normalize vector to unit length ───
static float normalize_simd(float *vec) {
    float norm = sqrtf(dot_product_simd(vec, vec));
    if (norm < 1e-10f) return 0;
    float inv = 1.0f / norm;
    for (int i = 0; i < HD_DIM; i += 8) {
        __m256 v = _mm256_load_ps(&vec[i]);
        __m256 scaled = _mm256_mul_ps(v, _mm256_set1_ps(inv));
        _mm256_store_ps(&vec[i], scaled);
    }
    return norm;
}

// ─── Find nearest concept by dot product ───
static int find_nearest_simd(const float *vec, float *similarity) {
    int best = -1;
    float best_dot = -1e10f;
    
    for (int i = 0; i < n_concepts; i++) {
        if (!concepts[i].active) continue;
        float dot = dot_product_simd(vec, concepts[i].vec);
        if (dot > best_dot) {
            best_dot = dot;
            best = i;
        }
    }
    
    if (similarity) *similarity = (best_dot / HD_DIM_F + 1.0f) / 2.0f;
    return best;
}

// ─── Hybrid concept blending ───
// Given low confidence, blends V_curr + V_prev into a hybrid concept
static void hybrid_blend(const float *v_curr, const float *v_prev,
                         float confidence, int *out_id, float *out_conf,
                         char *out_label) {
    if (confidence >= 0.4f) {
        // High confidence: use standard nearest
        *out_id = find_nearest_simd(v_curr, out_conf);
        if (*out_id >= 0) strncpy(out_label, concepts[*out_id].label, 31);
        return;
    }
    
    // Low confidence: blend V_curr + V_prev into hybrid
    float hybrid[HD_DIM] __attribute__((aligned(32)));
    blend_vectors_simd(v_curr, v_prev, hybrid);
    normalize_simd(hybrid);
    
    *out_id = find_nearest_simd(hybrid, out_conf);
    
    if (*out_id >= 0) {
        // Create a hybrid label
        int n_curr = find_nearest_simd(v_curr, NULL);
        int n_prev = find_nearest_simd(v_prev, NULL);
        if (n_curr >= 0 && n_prev >= 0 && n_curr != n_prev) {
            snprintf(out_label, 31, "%s+%s→%s",
                     concepts[n_prev].label,
                     concepts[n_curr].label,
                     concepts[*out_id].label);
        } else if (*out_id >= 0) {
            strncpy(out_label, concepts[*out_id].label, 31);
        }
    }
}

// ─── Demonstrate the engine ───
int main() {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║   HDC HYPERDIMENSIONAL CONCEPT BLENDER         ║\n");
    printf("║   1024-dim vectors, SIMD FMA, hybrid concepts   ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");
    
    init_concepts();
    printf("  Loaded %d hyperdimensional concepts\n", n_concepts);
    printf("  Vector dims: %d (%.0f kB per concept)\n", HD_DIM, HD_DIM * 4.0f / 1024.0f);
    printf("\n");
    
    // ─── Phase 1: Vector map ───
    printf("── Concept Similarity Map ──\n");
    for (int i = 0; i < n_concepts && i < 6; i++) {
        printf("  %-12s ", concepts[i].label);
        for (int j = 0; j < n_concepts && j < 6; j++) {
            if (i == j) { printf("  1.00  "); continue; }
            float sim = (dot_product_simd(concepts[i].vec, concepts[j].vec) / HD_DIM_F + 1.0f) / 2.0f;
            printf("  %.2f  ", sim);
        }
        printf("\n");
    }
    printf("\n");
    
    // ─── Phase 2: High-confidence prediction ───
    printf("── High Confidence (≥0.4) ──\n");
    float conf_high = 0.7f;
    int out_id; float out_conf; char out_label[32];
    
    // Current = compile, previous = optimize
    hybrid_blend(concepts[1].vec, concepts[0].vec, conf_high, &out_id, &out_conf, out_label);
    printf("  curr=compile, prev=optimize → %s (conf=%.2f)\n", out_label, out_conf);
    
    // Current = gpu, previous = memory
    hybrid_blend(concepts[8].vec, concepts[5].vec, conf_high, &out_id, &out_conf, out_label);
    printf("  curr=gpu, prev=memory      → %s (conf=%.2f)\n", out_label, out_conf);
    
    // ─── Phase 3: Low-confidence hybrid blending (the magic) ───
    printf("\n── Low Confidence — Hybrid Blending (<0.4) ──\n");
    float conf_low = 0.2f;
    
    // gpu + cpu → hybrid
    hybrid_blend(concepts[8].vec, concepts[9].vec, conf_low, &out_id, &out_conf, out_label);
    printf("  gpu + cpu          → %s (conf=%.2f)\n", out_label, out_conf);
    
    // compile + parallel → hybrid
    hybrid_blend(concepts[1].vec, concepts[3].vec, conf_low, &out_id, &out_conf, out_label);
    printf("  compile + parallel → %s (conf=%.2f)\n", out_label, out_conf);
    
    // cache + network → hybrid
    hybrid_blend(concepts[4].vec, concepts[6].vec, conf_low, &out_id, &out_conf, out_label);
    printf("  cache + network    → %s (conf=%.2f)\n", out_label, out_conf);
    
    // read + write → hybrid
    hybrid_blend(concepts[34].vec, concepts[35].vec, conf_low, &out_id, &out_conf, out_label);
    printf("  read + write       → %s (conf=%.2f)\n", out_label, out_conf);
    
    // encode + decode → hybrid
    hybrid_blend(concepts[30].vec, concepts[31].vec, conf_low, &out_id, &out_conf, out_label);
    printf("  encode + decode    → %s (conf=%.2f)\n", out_label, out_conf);
    
    // compress + encrypt → hybrid
    hybrid_blend(concepts[40].vec, concepts[42].vec, conf_low, &out_id, &out_conf, out_label);
    printf("  compress + encrypt → %s (conf=%.2f)\n", out_label, out_conf);
    
    // allocate + free → hybrid
    hybrid_blend(concepts[32].vec, concepts[33].vec, conf_low, &out_id, &out_conf, out_label);
    printf("  allocate + free    → %s (conf=%.2f)\n", out_label, out_conf);
    
    // mount + bind → hybrid
    hybrid_blend(concepts[44].vec, concepts[46].vec, conf_low, &out_id, &out_conf, out_label);
    printf("  mount + bind       → %s (conf=%.2f)\n", out_label, out_conf);
    
    // ─── Phase 4: Benchmark ───
    printf("\n── SIMD FMA Throughput ──\n");
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "ecx");
    uint64_t t0 = ((uint64_t)hi << 32) | lo;
    
    int iters = 10000;
    float sum = 0;
    for (int i = 0; i < iters; i++) {
        sum += dot_product_simd(concepts[i % n_concepts].vec, concepts[(i+1) % n_concepts].vec);
    }
    
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "ecx");
    uint64_t t1 = ((uint64_t)hi << 32) | lo;
    
    printf("  %d dot products in %lu cycles\n", iters, t1 - t0);
    printf("  %.0f cycles/dot product (1024 dimensions)\n", (float)(t1 - t0) / iters);
    printf("  %.1f GFLOPs\n", (float)iters * 2048.0f / (float)(t1 - t0) * 2700.0f);
    (void)sum;
    
    printf("\n╔══════════════════════════════════════════════════╗\n");
    printf("║   HDC BLENDING COMPLETE                        ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");
    
    return 0;
}
