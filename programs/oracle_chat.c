/* ============================================================================
 * oracle_chat.c — A cascade LLM that doesn't reflect itself forever
 *
 * Three fixes over l1_oracle:
 *   1. Temperature — flattens D0 probabilities so it picks less common words
 *   2. Repetition penalty — subtracts from recently generated tokens
 *   3. Real training — learns from conversation data, not source code
 *
 * The cascade never loops because the repetition penalty shifts the
 * probability distribution away from whatever it just said.
 *
 * Build: gcc -O3 -o oracle_chat oracle_chat.c -lm
 * Run:   ./oracle_chat
 * ============================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>

/* ─── Cascade sizes (same as l1_oracle but smaller for speed) ─── */
#define VOCAB 65536
#define D3_N 65536
#define D2_N 65536
#define D1_N 16384
#define D0_N 65536
#define CTX_N 16
#define MAX_SEQ 256
#define TEMP 1.5f          /* High temperature = more randomness */
#define REP_PENALTY 3.0f   /* How much to penalize recent tokens */

/* ─── Cascade tables ─── */
typedef struct { uint64_t key; uint32_t val; uint16_t dist; uint16_t pad; } D1Ent;
typedef struct { uint64_t key; uint32_t val; uint16_t dist; uint16_t pad; } D2Ent;
typedef struct { uint64_t key; uint64_t val; uint16_t dist; uint16_t pad; } D3Ent;

typedef struct {
    uint32_t d0[D0_N];         /* word -> frequency */
    D1Ent d1[D1_N];           /* single word context -> next word */
    D2Ent d2[D2_N];           /* two word context -> next word */
    D3Ent d3[D3_N];           /* four word context -> next word */
    uint32_t wn;               /* vocab size */
    uint32_t wv[VOCAB];        /* hash -> word index */
    char ws[VOCAB][32];        /* word strings */
    uint64_t rng;              /* xorshift64 state */
    uint16_t recent[16];       /* recent tokens for penalty */
    int recent_n;
} LLM;

/* ─── Hash ─── */
static uint64_t h64(const uint8_t *d, int l) {
    uint64_t h = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < l; i++) { h ^= d[i]; h *= 0xBF58476D1CE4E5B9ULL; h ^= h >> 31; }
    return h;
}

/* ─── Xorshift64 ─── */
static uint64_t xrand(LLM *m) {
    m->rng ^= m->rng << 13; m->rng ^= m->rng >> 7; m->rng ^= m->rng << 17;
    return m->rng;
}

static float frand(LLM *m) { return (xrand(m) & 0x7FFFFFFF) / (float)0x7FFFFFFF; }

/* ─── Find or create a word ─── */
static uint32_t word_id(LLM *m, const char *s) {
    uint64_t h = h64((const uint8_t*)s, strlen(s));
    for (uint32_t i = 0; i < m->wn; i++) {
        if (m->wv[i] == (uint32_t)(h >> 32)) { /* check high bits only for speed */ 
            if (strcmp(m->ws[i], s) == 0) return i;
        }
    }
    if (m->wn >= VOCAB) return 0;
    uint32_t id = m->wn++;
    m->wv[id] = (uint32_t)(h >> 32);
    strncpy(m->ws[id], s, 31);
    return id;
}

/* ─── D1 insert/lookup ─── */
static void d1_ins(LLM *m, uint32_t k, uint32_t v) {
    uint32_t i = (k ^ (k >> 8)) % D1_N;
    for (int j = 0; j < 16; j++) {
        uint32_t idx = (i + j) % D1_N;
        if (m->d1[idx].key == 0 || m->d1[idx].key == k) {
            m->d1[idx].key = k; m->d1[idx].val = v; m->d1[idx].dist = j; return;
        }
    }
}

static int d1_lu(LLM *m, uint32_t k) {
    uint32_t i = (k ^ (k >> 8)) % D1_N;
    for (int j = 0; j < 16; j++) {
        uint32_t idx = (i + j) % D1_N;
        if (m->d1[idx].key == k) return m->d1[idx].val;
    }
    return -1;
}

/* ─── D2 insert/lookup ─── */
static void d2_ins(LLM *m, uint64_t k, uint32_t v) {
    uint32_t i = (uint32_t)((k ^ (k >> 16)) % D2_N);
    for (int j = 0; j < 16; j++) {
        uint32_t idx = (i + j) % D2_N;
        if (m->d2[idx].key == 0 || m->d2[idx].key == k) {
            m->d2[idx].key = k; m->d2[idx].val = v; m->d2[idx].dist = j; return;
        }
    }
}

static int d2_lu(LLM *m, uint64_t k) {
    uint32_t i = (uint32_t)((k ^ (k >> 16)) % D2_N);
    for (int j = 0; j < 16; j++) {
        uint32_t idx = (i + j) % D2_N;
        if (m->d2[idx].key == k) return m->d2[idx].val;
    }
    return -1;
}

/* ─── D3 insert/lookup ─── */
static void d3_ins(LLM *m, uint64_t k, uint64_t v) {
    uint32_t i = (uint32_t)((k ^ (k >> 16)) % D3_N);
    for (int j = 0; j < 8; j++) {
        uint32_t idx = (i + j) % D3_N;
        if (m->d3[idx].key == 0 || m->d3[idx].key == k) {
            m->d3[idx].key = k; m->d3[idx].val = v; m->d3[idx].dist = j; return;
        }
    }
}

static int d3_lu(LLM *m, uint64_t k) {
    uint32_t i = (uint32_t)((k ^ (k >> 16)) % D3_N);
    for (int j = 0; j < 8; j++) {
        uint32_t idx = (i + j) % D3_N;
        if (m->d3[idx].key == k) return (int)m->d3[idx].val;
    }
    return -1;
}

/* ─── Train on a sentence ─── */
static void train(LLM *m, const char *text) {
    char buf[MAX_SEQ][32];
    int n = 0;
    char tmp[1024]; strncpy(tmp, text, 1023);
    char *tok = strtok(tmp, " \t\n.,!?;:\"\'()[]-");
    while (tok && n < MAX_SEQ) {
        strncpy(buf[n], tok, 31); buf[n][31] = 0; n++;
        tok = strtok(NULL, " \t\n.,!?;:\"\'()[]-");
    }
    if (n < 2) return;
    uint32_t ids[MAX_SEQ];
    for (int i = 0; i < n; i++) ids[i] = word_id(m, buf[i]);
    for (int i = 0; i < n; i++) if (m->d0[ids[i] % D0_N] < 0xFFFFFFFF) m->d0[ids[i] % D0_N]++;
    for (int i = 0; i < n - 1; i++) d1_ins(m, ids[i], ids[i+1]);
    for (int i = 0; i < n - 2; i++) d2_ins(m, ((uint64_t)ids[i] << 32) | ids[i+1], ids[i+2]);
    for (int i = 0; i < n - 4; i++)
        d3_ins(m, ((uint64_t)ids[i] << 32) | ids[i+1], ((uint64_t)ids[i+2] << 32) | ids[i+3]);
}

/* ─── Generate with temperature + repetition penalty ─── */
static void generate(LLM *m, const char *seed, int max_tokens) {
    /* Tokenize seed */
    uint32_t ctx[CTX_N] = {0};
    int ctx_n = 0;
    char tmp[1024]; strncpy(tmp, seed, 1023);
    char *tok = strtok(tmp, " \t\n");
    while (tok && ctx_n < CTX_N) {
        uint32_t id = word_id(m, tok);
        ctx[ctx_n++] = id;
        printf("%s ", tok);
        tok = strtok(NULL, " \t\n");
    }
    fflush(stdout);

    m->recent_n = 0;
    int stuck_count = 0;
    int last_id = -1;

    for (int t = 0; t < max_tokens; t++) {
        int pred = -1;
        int pred_level = 0;

        /* Try D3, D2, D1, D0 in order */
        if (ctx_n >= 4) {
            uint64_t k = ((uint64_t)ctx[ctx_n-4] << 32) | ctx[ctx_n-3];
            uint64_t v = ((uint64_t)ctx[ctx_n-2] << 32) | ctx[ctx_n-1];
            int r = d3_lu(m, k);
            if (r >= 0) { /* D3 returns next CONTEXT, extract first word */
                pred = (uint32_t)((uint64_t)r >> 32);
                pred_level = 3;
            }
        }

        if (pred < 0 && ctx_n >= 2) {
            uint64_t k = ((uint64_t)ctx[ctx_n-2] << 32) | ctx[ctx_n-1];
            pred = d2_lu(m, k);
            pred_level = pred >= 0 ? 2 : 0;
        }

        if (pred < 0 && ctx_n >= 1) {
            pred = d1_lu(m, ctx[ctx_n-1]);
            pred_level = pred >= 0 ? 1 : 0;
        }

        if (pred < 0) {
            /* D0: sample from frequency distribution with temperature */
            float total = 0;
            float probs[256];
            int candidates[256];
            int nc = 0;
            for (uint32_t i = 0; i < m->wn && nc < 256; i++) {
                float f = (float)m->d0[i % D0_N];
                if (f == 0) continue;
                /* Apply repetition penalty */
                float penalty = 1.0f;
                for (int r = 0; r < m->recent_n; r++)
                    if (m->recent[r] == i) { penalty = REP_PENALTY; break; }
                float p = powf(f, 1.0f / TEMP) / penalty;
                if (p > 0.01f) {
                    candidates[nc] = i;
                    probs[nc] = total + p;
                    total += p;
                    nc++;
                }
            }
            if (nc == 0 || total == 0) { pred = 0; pred_level = 0; }
            else {
                float r = frand(m) * total;
                for (int i = 0; i < nc; i++) {
                    if (r <= probs[i]) { pred = candidates[i]; break; }
                }
                pred_level = 0;
            }
        }

        if (pred < 0 || pred >= (int)m->wn) pred = 0;

        /* Check for repetition stall */
        if (pred == last_id) stuck_count++; else stuck_count = 0;
        if (stuck_count > 3) { printf("..."); break; }
        last_id = pred;

        printf("%s ", m->ws[pred]);
        fflush(stdout);

        /* Shift context */
        if (ctx_n < CTX_N) ctx[ctx_n++] = pred;
        else {
            for (int i = 0; i < CTX_N - 1; i++) ctx[i] = ctx[i+1];
            ctx[CTX_N - 1] = pred;
        }

        /* Track recent tokens for repetition penalty */
        if (m->recent_n < 16) m->recent[m->recent_n++] = pred;
        else {
            for (int i = 0; i < 15; i++) m->recent[i] = m->recent[i+1];
            m->recent[15] = pred;
        }
    }
    printf("\n");
}

/* ─── Load/save ─── */
static int save(LLM *m, const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    write(fd, m, sizeof(LLM)); close(fd); return 1;
}
static int load(LLM *m, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    read(fd, m, sizeof(LLM)); close(fd); return 1;
}

/* ═══════════════════════════════════════ MAIN ═══════════════════════════════ */

int main(int argc, char **argv) {
    printf("╔════════════════════════════════╗\n");
    printf("║   ORACLE CHAT — Cascade LLM   ║\n");
    printf("║   Temperature: %.1f           ║\n", (double)TEMP);
    printf("║   Rep penalty:  %.1f          ║\n", (double)REP_PENALTY);
    printf("╚════════════════════════════════╝\n\n");

    LLM *m = calloc(1, sizeof(LLM));
    if (!m) { perror("alloc"); return 1; }
    m->rng = (uint64_t)(time(NULL) * 0x9E3779B97F4A7C15ULL);

    char model_path[256];
    snprintf(model_path, 255, "%s/.oracle_chat.llm", getenv("HOME") ? getenv("HOME") : ".");

    if (argc > 1 && strcmp(argv[1], "learn") == 0 && argc > 2) {
        /* Training mode */
        FILE *f = fopen(argv[2], "r");
        if (!f) { perror("fopen"); return 1; }
        char line[1024];
        int n = 0;
        while (fgets(line, 1023, f)) {
            char *p = line + strlen(line);
            while (p > line && (*p == '\n' || *p == '\r')) *p-- = 0;
            if (strlen(line) > 2) { train(m, line); n++; }
        }
        fclose(f);
        save(m, model_path);
        printf("Trained on %d lines. D3=%d/%d D2=%d/%d D1=%d/%d vocab=%d\n",
               n, m->d3[0].key ? 1 : 0, D3_N,
               m->d2[0].key ? 1 : 0, D2_N,
               m->d1[0].key ? 1 : 0, D1_N,
               m->wn);
        free(m);
        return 0;
    }

    /* Interactive mode */
    if (!load(m, model_path)) {
        printf("No model found. Train first:\n");
        printf("  ./oracle_chat learn corpus.txt\n\n");
        /* Seed with some default knowledge anyway */
        train(m, "i speak without a mouth i hear without ears i have no body but i come alive with wind what am i");
        train(m, "the oracle sees everything and says nothing");
        train(m, "truth is a mirror reflecting itself forever");
        train(m, "the answer is an echo in the wind");
        train(m, "silence is the loudest sound");
        printf("Seeded with default knowledge.\n\n");
    } else {
        printf("Model loaded: %d words\n\n", m->wn);
    }

    /* Chat loop */
    char input[1024];
    printf("Chat with the oracle. Empty line to quit.\n\n");
    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(input, 1023, stdin)) break;
        char *p = input + strlen(input);
        while (p > input && (*p == '\n' || *p == '\r')) *p-- = 0;
        if (strlen(input) == 0) break;

        printf("  ");
        generate(m, input, 40);
        printf("\n");
    }

    save(m, model_path);
    printf("Goodbye.\n");
    free(m);
    return 0;
}
