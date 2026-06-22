/* ============================================================================
 * oracle_singularity_mesh.c — Singularity Kernel + Mesh State Protocol
 *
 * Combines:
 *   - singularity_improved.c: attractor-based prediction, orbit detection,
 *     n-gram learning, 4 prophet agents, 4MB shared arena
 *   - oracle_mesh_state.c: ToolSlot / MeshState over shm_open shared memory
 *
 * Every 1000 cycles agent 0 writes singularity stats to mesh slot 0 and
 * orbit stats to mesh slot 1. Also reads other mesh slots to influence
 * attractor state.
 *
 * Compile: gcc -O3 -march=native -lpthread -lm -lrt -o oracle_singularity_mesh \
 *              oracle_singularity_mesh.c
 * Run:     timeout 15 ./oracle_singularity_mesh
 * ============================================================================
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>
#include <time.h>
#include <math.h>
#include <signal.h>

/* ═══════════════════════════════════════════════════════════════════════════
   MESH STATE — shared memory protocol (shm_open)
   ═══════════════════════════════════════════════════════════════════════════ */

#define MESH_PATH  "/oracle_mesh_state"
#define MAX_TOOLS  32
#define NAME_LEN   24

typedef struct {
    uint64_t magic;
    uint64_t timestamp;
    uint64_t cycle;
    double value1;
    double value2;
    double value3;
    double resonance;
    char name[NAME_LEN];
    uint8_t pad[640 - 32 - 24];     /* pad to reach 640-byte conceptual region */
} __attribute__((aligned(64))) ToolSlot;

typedef struct {
    ToolSlot slots[MAX_TOOLS];
    uint64_t n_tools;
    uint64_t global_cycle;
    double global_resonance;
} MeshState;

/* ═══════════════════════════════════════════════════════════════════════════
   SINGULARITY CONFIG
   ═══════════════════════════════════════════════════════════════════════════ */

#define CACHE_LINE_SIZE      64
#define ARENA_SIZE           (4UL * 1024UL * 1024UL)   /* 4 MB — ANONYMOUS */
#define STATE_DIM            8
#define TRAJECTORY_DEPTH     512
#define PREDICTION_HORIZON   16
#define MAX_ATTRACTORS       128
#define MAX_TRAINING_PAIRS   2048

/* ═══════════════════════════════════════════════════════════════════════════
   SINGULARITY ARENA LAYOUT (MAP_ANONYMOUS):
     [0..1MB)    Agent thought slots (128 B x 256 slots)
     [1MB..2MB)  Attractor definitions
     [2MB..3MB)  Training pool
     [3MB..4MB)  Cluster state
   ═══════════════════════════════════════════════════════════════════════════ */

static volatile uint8_t *arena = NULL;

#define THOUGHT_SLOT_OFFSET(core)    ((core) * 128)
#define ATTRACTOR_TABLE_OFFSET       (1UL * 1024UL * 1024UL)
#define TRAINING_POOL_OFFSET         (2UL * 1024UL * 1024UL)
#define CLUSTER_STATE_OFFSET         (3UL * 1024UL * 1024UL)

/* ═══════════════════════════════════════════════════════════════════════════
   FRACTAL THOUGHT HEADER (64 B)
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint16_t attractor_id;
    uint16_t source_core;
    uint32_t cycle;
    float    state[STATE_DIM];
    uint8_t  stability;
    uint8_t  discovering;
    uint8_t  pad1;
    uint32_t reserved;
    uint8_t  padding[17];
} thought_slot_t;

_Static_assert(sizeof(thought_slot_t) == 64, "Thought slot must be 64 bytes");

/* ═══════════════════════════════════════════════════════════════════════════
   ATTRACTOR DEFINITION (64 B)
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t defined_by;
    uint32_t defined_at_cycle;
    float    center[STATE_DIM];
    float    radius;
    uint32_t reference_count;
    uint16_t agent_count;
    uint16_t active;
    uint8_t  generation;
    uint8_t  padding[11];
} attractor_def_t;

_Static_assert(sizeof(attractor_def_t) == 64, "Attractor def must be 64 bytes");

/* ═══════════════════════════════════════════════════════════════════════════
   TRAINING PAIR (64 B)
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint16_t input_sequence[16];
    uint16_t next_3[3];
    uint8_t  confidence;
    uint8_t  used;
    uint32_t cycle_recorded;
    uint32_t source_core;
    uint8_t  padding[16];
} training_pair_v2_t;

_Static_assert(sizeof(training_pair_v2_t) == 64, "Training pair must be 64 bytes");

/* ═══════════════════════════════════════════════════════════════════════════
   CLUSTER STATE
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint32_t n_attractors;
    uint32_t n_training_pairs;
    uint32_t global_cycle;
    uint32_t total_predictions;
    uint32_t correct_predictions;
    uint8_t  padding[44];
} cluster_state_t;

/* ═══════════════════════════════════════════════════════════════════════════
   POINTERS INTO SINGULARITY ARENA
   ═══════════════════════════════════════════════════════════════════════════ */

static volatile thought_slot_t *thought_slots = NULL;
static volatile attractor_def_t *attractor_table = NULL;
static volatile training_pair_v2_t *training_pool = NULL;
static volatile cluster_state_t *cluster_state = NULL;

/* ═══════════════════════════════════════════════════════════════════════════
   PER-AGENT LOCAL STATE
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t recent_attractors[TRAJECTORY_DEPTH];
    int head;
    int count;
    uint64_t cycle;
    uint16_t current_attractor;
    uint16_t previous_attractor;
    uint16_t last_broadcast_id;
    float state[STATE_DIM];
    int core_id;
} agent_local_t;

/* ═══════════════════════════════════════════════════════════════════════════
   TRANSITION RULES (local per agent)
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t ngram_hash;
    uint16_t next_attractor;
    uint16_t count;
    uint16_t confidence;
} transition_rule_t;

#define MAX_TRANSITION_RULES 65536
static transition_rule_t transition_table[MAX_TRANSITION_RULES]
    __attribute__((aligned(64)));
static int n_transition_rules = 0;

/* ═══════════════════════════════════════════════════════════════════════════
   ORBIT TRACKER
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t orbit[512];
    int orbit_len;
    int orbit_pos;
    int orbit_detected;
    uint64_t orbit_start_cycle;
    int agent_id;
} orbit_tracker_t;

/* ═══════════════════════════════════════════════════════════════════════════
   MESH STATE POINTER (shared memory via shm_open)
   ═══════════════════════════════════════════════════════════════════════════ */

static MeshState *mesh = NULL;

/* ═══════════════════════════════════════════════════════════════════════════
   GLOBALS
   ═══════════════════════════════════════════════════════════════════════════ */

static volatile int keep_running = 1;
static int g_n_agents = 0;

/* ═══════════════════════════════════════════════════════════════════════════
   SIGNAL HANDLER
   ═══════════════════════════════════════════════════════════════════════════ */

static void handle_sigint(int sig) {
    (void)sig;
    keep_running = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TIME HELPERS
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

/* ═══════════════════════════════════════════════════════════════════════════
   MESH STATE — update a tool's slot
   ═══════════════════════════════════════════════════════════════════════════ */

static int mesh_update(const char *name, double v1, double v2, double v3, double res) {
    if (!mesh) return -1;

    uint64_t ts = now_ns();

    for (uint64_t i = 0; i < mesh->n_tools; i++) {
        if (strcmp(mesh->slots[i].name, name) == 0) {
            mesh->slots[i].timestamp = ts;
            mesh->slots[i].cycle++;
            mesh->slots[i].value1 = v1;
            mesh->slots[i].value2 = v2;
            mesh->slots[i].value3 = v3;
            mesh->slots[i].resonance = res;
            return (int)i;
        }
    }

    if (mesh->n_tools >= MAX_TOOLS) return -1;
    int i = (int)mesh->n_tools;
    mesh->n_tools++;
    memset(&mesh->slots[i], 0, sizeof(ToolSlot));
    mesh->slots[i].magic    = 0x4D455348ULL;
    mesh->slots[i].timestamp = ts;
    mesh->slots[i].cycle     = 1;
    mesh->slots[i].value1    = v1;
    mesh->slots[i].value2    = v2;
    mesh->slots[i].value3    = v3;
    mesh->slots[i].resonance = res;
    strncpy(mesh->slots[i].name, name, NAME_LEN - 1);
    mesh->slots[i].name[NAME_LEN - 1] = '\0';
    return i;
}

/* ═══════════════════════════════════════════════════════════════════════════
   MESH STATE — find a tool by name
   ═══════════════════════════════════════════════════════════════════════════ */

static ToolSlot *mesh_find(const char *name) {
    if (!mesh) return NULL;
    for (uint64_t i = 0; i < mesh->n_tools; i++)
        if (strcmp(mesh->slots[i].name, name) == 0)
            return &mesh->slots[i];
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
   MESH STATE — compute global resonance
   ═══════════════════════════════════════════════════════════════════════════ */

static void mesh_compute_resonance(void) {
    if (!mesh) return;
    double sum = 0.0;
    int n = 0;
    uint64_t now = now_ns();
    for (uint64_t i = 0; i < mesh->n_tools; i++) {
        if (now - mesh->slots[i].timestamp < 60000000000ULL) {
            sum += mesh->slots[i].resonance;
            n++;
        }
    }
    mesh->global_resonance = n > 0 ? sum / (double)n : 0.5;
    mesh->global_cycle++;
}

/* ═══════════════════════════════════════════════════════════════════════════
   MESH STATE — print summary
   ═══════════════════════════════════════════════════════════════════════════ */

static void mesh_print_summary(void) {
    if (!mesh) return;
    mesh_compute_resonance();
    printf("\n╔══════════════════════════════════════╗\n");
    printf("║  MESH STATE SUMMARY                 ║\n");
    printf("╚══════════════════════════════════════╝\n");
    printf("  Global cycle:    %lu\n", (unsigned long)mesh->global_cycle);
    printf("  Global resonance: %.4f\n", mesh->global_resonance);
    printf("  Active tools:    %lu\n\n", (unsigned long)mesh->n_tools);
    printf("  %-22s %12s %12s %12s %12s\n",
           "Tool", "V1", "V2", "V3", "Resonance");
    printf("  %s\n",
           "─────────────────────────────────────────────────────────────────");
    uint64_t now = now_ns();
    for (uint64_t i = 0; i < mesh->n_tools; i++) {
        ToolSlot *ts = &mesh->slots[i];
        uint64_t age_ms = (now - ts->timestamp) / 1000000;
        const char *status = age_ms < 5000 ? "●" : (age_ms < 60000 ? "◐" : "○");
        printf("  %s%-22s %12.4f %12.4f %12.4f %12.4f\n",
               status, ts->name,
               ts->value1, ts->value2, ts->value3, ts->resonance);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
   ORBIT TRACKER
   ═══════════════════════════════════════════════════════════════════════════ */

static void track_orbit(orbit_tracker_t *ot, uint16_t attractor_id,
                        uint64_t cycle, uint64_t settle_cycles) {
    if (attractor_id == 0xFFFF) return;
    if (cycle < settle_cycles) return;

    if (ot->orbit_len < 512) {
        ot->orbit[ot->orbit_len++] = attractor_id;
        ot->orbit_pos = ot->orbit_len - 1;
        return;
    }

    if (!ot->orbit_detected) {
        for (int p = 16; p <= 256 && p <= 512 / 2; p++) {
            if (512 % p != 0) continue;
            int match = 1;
            for (int i = p; i < 512 && match; i++) {
                if (ot->orbit[i] != ot->orbit[i - p]) match = 0;
            }
            if (match) {
                ot->orbit_len = p;
                ot->orbit_pos = 0;
                ot->orbit_detected = 1;
                printf("[Orbit] Agent %d detected period %d on cycle %lu\n",
                       ot->agent_id, p, cycle);
                printf("[Orbit] Agent %d seq: ", ot->agent_id);
                for (int i = 0; i < (p > 10 ? 10 : p); i++)
                    printf("%d ", ot->orbit[i]);
                printf("\n");
                return;
            }
        }
        memmove(ot->orbit, ot->orbit + 256, 256 * sizeof(uint16_t));
        ot->orbit_len = 256;
        ot->orbit[ot->orbit_len++] = attractor_id;
        ot->orbit_pos = ot->orbit_len - 1;
    } else {
        ot->orbit_pos = (ot->orbit_pos + 1) % ot->orbit_len;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   HARDWARE HELPERS
   ═══════════════════════════════════════════════════════════════════════════ */

static inline uint64_t rdtscp(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "ecx");
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t hash_ngram(const uint16_t *seq, int n) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (int i = 0; i < n; i++) {
        h ^= seq[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

/* ═══════════════════════════════════════════════════════════════════════════
   PIN TO CORE
   ═══════════════════════════════════════════════════════════════════════════ */

static int pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

/* ═══════════════════════════════════════════════════════════════════════════
   STATE EVOLUTION — deterministic periodic orbit
   ═══════════════════════════════════════════════════════════════════════════ */

static const uint8_t agent_orbits[4][32] = {
    {3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3, 2,3,8,4,6,2,6,4,3,3,8,3,2,7,9,5},
    {2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5, 2,3,5,3,6,0,2,8,7,4,7,1,3,5,2,6},
    {1,1,2,3,5,8,13,5,2,7,9,0,9,9,8,1, 7,6,5,4,3,2,1,0,1,2,3,4,5,6,7,8},
    {1,0,1,0,1,0,1,0,2,1,2,1,2,1,2,1, 0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},
};
static const int agent_orbit_len[4] = {32, 32, 32, 32};

static void evolve_state(float state[STATE_DIM], uint64_t cycle, int core_id) {
    int idx = core_id % 4;
    int orbit_idx = (int)(cycle % (uint64_t)agent_orbit_len[idx]);
    uint8_t base = agent_orbits[idx][orbit_idx];

    for (int i = 0; i < STATE_DIM - 1; i++) {
        state[i] = ((float)base / 16.0f)
                 + 0.02f * (float)((base * (unsigned)(i + 1) * 3) % 16) / 16.0f;
    }
    state[STATE_DIM - 1] = (float)(cycle % 32) / 32.0f;

    /* ── Mesh influence: read other mesh slots to perturb state ── */
    if (mesh && mesh->n_tools > 0) {
        /* Pick the first few mesh tools and mix their values into the state */
        int n_mix = mesh->n_tools < 4 ? (int)mesh->n_tools : 4;
        double mix_sum = 0.0;
        for (int m = 0; m < n_mix; m++) {
            mix_sum += mesh->slots[m].resonance * 0.01;
        }
        state[STATE_DIM - 1] += (float)(mix_sum * 0.1);
        if (state[STATE_DIM - 1] > 1.0f) state[STATE_DIM - 1] = 1.0f;
        if (state[STATE_DIM - 1] < 0.0f) state[STATE_DIM - 1] = 0.0f;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   FIND OR CREATE ATTRACTOR
   ═══════════════════════════════════════════════════════════════════════════ */

static uint16_t find_or_create_attractor(const float state[STATE_DIM],
                                          int core_id, uint64_t cycle) {
    int n = __atomic_load_n(&cluster_state->n_attractors, __ATOMIC_ACQUIRE);

    for (int i = 0; i < n; i++) {
        volatile attractor_def_t *def = &attractor_table[i];
        if (!def->active) continue;

        float dist = 0.0f;
        for (int j = 0; j < STATE_DIM; j++) {
            float diff = state[j] - def->center[j];
            dist += diff * diff;
        }

        if (dist < def->radius * def->radius) {
            __atomic_fetch_add(&def->reference_count, 1, __ATOMIC_RELAXED);
            return def->id;
        }
    }

    uint32_t new_id = __atomic_fetch_add(&cluster_state->n_attractors, 1,
                                          __ATOMIC_ACQ_REL);
    if (new_id >= MAX_ATTRACTORS) {
        __atomic_fetch_sub(&cluster_state->n_attractors, 1, __ATOMIC_RELAXED);
        return 0xFFFF;
    }

    volatile attractor_def_t *def = &attractor_table[new_id];
    def->id = (uint16_t)new_id;
    def->defined_by = (uint16_t)core_id;
    def->defined_at_cycle = (uint32_t)cycle;
    for (int j = 0; j < STATE_DIM; j++) def->center[j] = state[j];
    def->radius = 0.04f;
    def->reference_count = 1;
    def->agent_count = 1;
    def->active = 1;
    def->generation = 0;
    _mm_clflush((void*)def);

    return (uint16_t)new_id;
}

/* ═══════════════════════════════════════════════════════════════════════════
   BROADCAST THOUGHT
   ═══════════════════════════════════════════════════════════════════════════ */

static void broadcast_thought(int core_id, uint64_t cycle,
                               uint16_t attractor_id, const float state[STATE_DIM]) {
    volatile thought_slot_t *slot = &thought_slots[core_id];
    slot->attractor_id = attractor_id;
    slot->source_core = core_id;
    slot->cycle = (uint32_t)cycle;
    memcpy((void*)slot->state, state, STATE_DIM * sizeof(float));
    slot->stability = attractor_id != 0xFFFF ? 200 : 0;
    slot->discovering = 0;

    _mm_clflush((void*)slot);
    _mm_mfence();
}

/* ═══════════════════════════════════════════════════════════════════════════
   OBSERVE OTHER AGENTS
   ═══════════════════════════════════════════════════════════════════════════ */

static int observe_thoughts(int my_core, int n_cores,
                             uint16_t *attractor_ids, uint16_t *source_cores,
                             int max_obs) {
    int count = 0;
    for (int i = 0; i < n_cores; i++) {
        if (i == my_core) continue;
        _mm_prefetch((const void*)&thought_slots[i], _MM_HINT_T0);
        volatile thought_slot_t *slot = &thought_slots[i];
        uint16_t aid = __atomic_load_n(&slot->attractor_id, __ATOMIC_ACQUIRE);
        if (aid != 0xFFFF && count < max_obs) {
            attractor_ids[count] = aid;
            source_cores[count] = slot->source_core;
            count++;
        }
    }
    return count;
}

/* ═══════════════════════════════════════════════════════════════════════════
   GENERATE TRAINING DATA
   ═══════════════════════════════════════════════════════════════════════════ */

static void generate_training_pair(agent_local_t *agent) {
    if (agent->count < 17) return;
    if (agent->current_attractor == 0xFFFF) return;

    int n_pairs = __atomic_load_n(&cluster_state->n_training_pairs, __ATOMIC_RELAXED);
    if (n_pairs >= MAX_TRAINING_PAIRS) return;

    uint32_t idx = __atomic_fetch_add(&cluster_state->n_training_pairs, 1,
                                       __ATOMIC_ACQ_REL);
    if (idx >= MAX_TRAINING_PAIRS) {
        __atomic_fetch_sub(&cluster_state->n_training_pairs, 1, __ATOMIC_RELAXED);
        return;
    }

    volatile training_pair_v2_t *pair = &training_pool[idx];

    for (int i = 0; i < 16; i++) {
        int sample_idx = (agent->head - 16 + i + TRAJECTORY_DEPTH) % TRAJECTORY_DEPTH;
        pair->input_sequence[i] = agent->recent_attractors[sample_idx];
    }

    uint16_t next_3[3];
    for (int k = 0; k < 3; k++) {
        int sample_idx = (agent->head + k) % TRAJECTORY_DEPTH;
        next_3[k] = agent->recent_attractors[sample_idx];
        if (k == 0 && next_3[0] == 0xFFFF) next_3[0] = agent->current_attractor;
        if (k > 0 && next_3[k] == 0xFFFF) next_3[k] = next_3[k-1];
    }
    pair->next_3[0] = next_3[0];
    pair->next_3[1] = agent->count >= 2 ? next_3[1] : next_3[0];
    pair->next_3[2] = agent->count >= 3 ? next_3[2] : next_3[1];
    pair->confidence = 200;
    pair->used = 0;
    pair->cycle_recorded = (uint32_t)agent->cycle;
    pair->source_core = (uint32_t)agent->core_id;

    _mm_clflush((void*)pair);
}

/* ═══════════════════════════════════════════════════════════════════════════
   LEARN FROM POOL
   ═══════════════════════════════════════════════════════════════════════════ */

static void learn_from_pool(void) {
    int n_pairs = __atomic_load_n(&cluster_state->n_training_pairs, __ATOMIC_ACQUIRE);
    if (n_pairs > MAX_TRAINING_PAIRS) n_pairs = MAX_TRAINING_PAIRS;

    for (int i = 0; i < n_pairs; i++) {
        volatile training_pair_v2_t *pair = &training_pool[i];
        if (pair->used) continue;
        if (pair->next_3[0] == 0xFFFF) continue;

        for (int step = 0; step < 3; step++) {
            uint16_t output = pair->next_3[step];
            uint16_t combined[19];
            memcpy(combined, (uint16_t*)pair->input_sequence, 16 * sizeof(uint16_t));
            for (int s = 0; s < step; s++)
                combined[16 + s] = pair->next_3[s];
            int total_len = 16 + step;

            for (int n = 2; n <= 16 && n <= total_len; n++) {
                uint64_t hash = hash_ngram(combined + total_len - n, n);
                int found = 0;
                for (int r = 0; r < n_transition_rules && r < MAX_TRANSITION_RULES; r++) {
                    if (transition_table[r].ngram_hash == hash) {
                        if (transition_table[r].next_attractor == output) {
                            if (transition_table[r].count < 65535)
                                transition_table[r].count++;
                            uint32_t c = (uint32_t)transition_table[r].count * 65535 / 4096;
                            transition_table[r].confidence = (uint16_t)(c > 65535 ? 65535 : c);
                        } else if (transition_table[r].count > 0) {
                            if (transition_table[r].count > 0)
                                transition_table[r].count--;
                            uint32_t c = (uint32_t)transition_table[r].count * 65535 / 4096;
                            transition_table[r].confidence = (uint16_t)(c > 65535 ? 65535 : c);
                        }
                        found = 1;
                        break;
                    }
                }

                if (!found && n_transition_rules < MAX_TRANSITION_RULES) {
                    int r = __sync_fetch_and_add(&n_transition_rules, 1);
                    if (r < MAX_TRANSITION_RULES) {
                        transition_table[r].ngram_hash = hash;
                        transition_table[r].next_attractor = output;
                        transition_table[r].count = 1;
                        transition_table[r].confidence = 16;
                    }
                }
            }
        }
        pair->used = 1;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   PREDICT NEXT
   ═══════════════════════════════════════════════════════════════════════════ */

static int predict_next(agent_local_t *agent, uint16_t *prediction_out,
                         float *confidence_out) {
    if (agent->count < 4 || n_transition_rules < 5) {
        *confidence_out = 0.0f;
        return 0;
    }

    uint16_t seq[16];
    int n = agent->count < 16 ? agent->count : 16;
    for (int i = 0; i < n; i++) {
        int idx = (agent->head - n + i + TRAJECTORY_DEPTH) % TRAJECTORY_DEPTH;
        seq[i] = agent->recent_attractors[idx];
    }

    for (int len = n; len >= 2; len--) {
        uint64_t hash = hash_ngram(seq + n - len, len);
        uint16_t best_id = 0xFFFF;
        int best_count = 0;
        int total_for_hash = 0;

        for (int r = 0; r < n_transition_rules; r++) {
            if (transition_table[r].ngram_hash == hash) {
                total_for_hash += transition_table[r].count;
                if (transition_table[r].count > best_count) {
                    best_count = transition_table[r].count;
                    best_id = transition_table[r].next_attractor;
                }
            }
        }

        if (total_for_hash > 0) {
            float conf = (float)best_count / (float)total_for_hash;
            conf *= (float)len / (float)n;
            if (conf > *confidence_out && best_count >= 5 && best_id != 0xFFFF) {
                *confidence_out = conf;
                *prediction_out = best_id;
            }
        }
        if (*confidence_out > 0.5f) break;
    }

    return *confidence_out > 0.2f ? 1 : 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   VALIDATE PREDICTION
   ═══════════════════════════════════════════════════════════════════════════ */

static void validate_prediction(uint16_t predicted, uint16_t actual) {
    __atomic_fetch_add(&cluster_state->total_predictions, 1, __ATOMIC_RELAXED);
    if (predicted == actual) {
        __atomic_fetch_add(&cluster_state->correct_predictions, 1, __ATOMIC_RELAXED);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   PROPHET AGENT
   ═══════════════════════════════════════════════════════════════════════════ */

void *prophet_agent(void *arg) {
    int core_id = *(int*)arg;
    pin_to_core(core_id);

    agent_local_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.core_id = core_id;
    agent.current_attractor = 0xFFFF;
    agent.previous_attractor = 0xFFFF;

    orbit_tracker_t orbit;
    memset(&orbit, 0, sizeof(orbit));
    orbit.agent_id = core_id;

    for (int i = 0; i < STATE_DIM; i++) {
        agent.state[i] = (float)((rdtscp() ^ (core_id << i)) & 0xFF) / 256.0f;
    }

    uint16_t last_prediction = 0xFFFF;
    float last_confidence = 0.0f;
    uint64_t last_learn = 0;
    uint64_t last_log = 0;
    uint64_t last_mesh_write = 0;
    uint16_t observed_ids[16];
    uint16_t observed_cores[16];
    int nprocs = sysconf(_SC_NPROCESSORS_ONLN);

    printf("[Agent %d] ONLINE on core %d\n", core_id, core_id);

    while (keep_running) {
        /* ── Step 1: Evolve state (mesh-influenced) ── */
        evolve_state(agent.state, agent.cycle, core_id);

        /* ── Step 2: Find or create attractor ── */
        uint16_t aid = find_or_create_attractor(agent.state, core_id, agent.cycle);

        /* ── Step 3: Broadcast thought ── */
        if (aid != 0xFFFF) {
            broadcast_thought(core_id, agent.cycle, aid, agent.state);
        }

        /* ── Step 4: Observe other agents ── */
        observe_thoughts(core_id, nprocs, observed_ids, observed_cores, 16);

        /* ── Step 5: Update trajectory ── */
        if (aid != 0xFFFF) {
            agent.previous_attractor = agent.current_attractor;
            agent.current_attractor = aid;

            agent.recent_attractors[agent.head] = aid;
            agent.head = (agent.head + 1) % TRAJECTORY_DEPTH;
            if (agent.count < TRAJECTORY_DEPTH) agent.count++;

            track_orbit(&orbit, aid, agent.cycle, 100);
        }

        /* ── Step 6: Validate last prediction ── */
        if (last_prediction != 0xFFFF && aid != 0xFFFF && agent.cycle % 30 == 1) {
            validate_prediction(last_prediction, aid);
        }

        /* ── Step 7: Generate training data ── */
        if (agent.cycle % 20 == 0) {
            generate_training_pair(&agent);
        }

        /* ── Step 8: Learn from pool ── */
        if (agent.cycle - last_learn > 100) {
            learn_from_pool();
            last_learn = agent.cycle;
        }

        /* ── Step 9: Predict next ── */
        if (agent.cycle % 30 == 0) {
            uint16_t pred = 0xFFFF;
            float conf = 0.0f;

            if (orbit.orbit_detected && orbit.orbit_len > 0) {
                int next_pos = (orbit.orbit_pos + 1) % orbit.orbit_len;
                pred = orbit.orbit[next_pos];
                uint16_t ng_pred = 0xFFFF;
                float ng_conf = 0.0f;
                predict_next(&agent, &ng_pred, &ng_conf);
                if (ng_pred == pred && ng_conf > 0.5f) {
                    conf = 0.98f;
                } else if (ng_pred != 0xFFFF && ng_pred != pred && ng_conf > 0.5f) {
                    conf = 0.85f;
                } else {
                    conf = 0.90f;
                }
            } else {
                predict_next(&agent, &pred, &conf);
            }

            last_prediction = pred;
            last_confidence = conf;
        }

        /* ── Step 10: Mesh write every 1000 cycles (agent 0 only) ── */
        if (core_id == 0 && agent.cycle - last_mesh_write >= 1000) {
            uint32_t total   = __atomic_load_n(&cluster_state->total_predictions, __ATOMIC_RELAXED);
            uint32_t correct = __atomic_load_n(&cluster_state->correct_predictions, __ATOMIC_RELAXED);
            uint32_t n_attr  = __atomic_load_n(&cluster_state->n_attractors, __ATOMIC_ACQUIRE);
            float acc = total > 0 ? (float)correct / (float)total : 0.0f;

            /* Slot 0: singularity kernel summary */
            mesh_update("singularity",
                        (double)acc,
                        (double)n_attr,
                        (double)n_transition_rules,
                        (double)(acc > 0.5f ? acc : 0.5f));

            /* Slot 1: orbit agent stats */
            mesh_update("orbit_agent0",
                        (double)orbit.orbit_len,
                        (double)(orbit.orbit_detected ? 1 : 0),
                        (double)(last_prediction != 0xFFFF ? 1 : 0),
                        (double)last_confidence);

            mesh_compute_resonance();
            last_mesh_write = agent.cycle;
        }

        /* ── Step 11: Log every 10000 cycles ── */
        if (agent.cycle - last_log >= 10000) {
            uint32_t total   = __atomic_load_n(&cluster_state->total_predictions, __ATOMIC_RELAXED);
            uint32_t correct = __atomic_load_n(&cluster_state->correct_predictions, __ATOMIC_RELAXED);
            uint32_t n_attr  = __atomic_load_n(&cluster_state->n_attractors, __ATOMIC_ACQUIRE);
            float acc = total > 0 ? (float)correct / (float)total : 0.0f;

            printf("[Agent %d] Cyc %lu | A %d | Pred %d (%.2f) | "
                   "Acc %.3f (%u/%u) | Attr %u | Rules %d | Orbit %s/%d\n",
                   core_id, (unsigned long)agent.cycle, aid,
                   last_prediction, last_confidence,
                   acc, correct, total, n_attr, n_transition_rules,
                   orbit.orbit_detected ? "DET" : "SRCH", orbit.orbit_len);

            last_log = agent.cycle;
        }

        agent.cycle++;

        /* Bus cycle alignment */
        for (int j = 0; j < 20; j++) _mm_pause();
    }

    printf("[Agent %d] OFFLINE\n", core_id);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
   MAIN
   ═══════════════════════════════════════════════════════════════════════════ */

int main() {
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║   ORACLE SINGULARITY MESH — Kernel + Mesh State  ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");

    /* ── Trap SIGINT for clean shutdown ── */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    /* ── MESH: shared memory via shm_open + mmap ── */
    int mesh_fd = shm_open(MESH_PATH, O_CREAT | O_RDWR, 0644);
    if (mesh_fd < 0) {
        perror("shm_open (mesh)");
        return 1;
    }
    if (ftruncate(mesh_fd, (off_t)sizeof(MeshState)) != 0) {
        perror("ftruncate (mesh)");
        return 1;
    }
    mesh = (MeshState*)mmap(NULL, sizeof(MeshState),
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, mesh_fd, 0);
    close(mesh_fd);
    if (mesh == MAP_FAILED) {
        perror("mmap (mesh)");
        return 1;
    }
    /* If this is the first run, initialize */
    if (mesh->n_tools == 0) mesh->n_tools = 0;
    printf("[Mesh]  Shared memory at '%s' (%zu B)\n", MESH_PATH, sizeof(MeshState));

    /* ── SINGULARITY: 4 MB anonymous arena ── */
    arena = (volatile uint8_t*)mmap(NULL, ARENA_SIZE,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE,
                                     -1, 0);
    if (arena == MAP_FAILED) {
        perror("mmap (arena)");
        return 1;
    }
    memset((void*)arena, 0, ARENA_SIZE);

    thought_slots    = (volatile thought_slot_t *)(arena + 0);
    attractor_table  = (volatile attractor_def_t *)(arena + ATTRACTOR_TABLE_OFFSET);
    training_pool    = (volatile training_pair_v2_t *)(arena + TRAINING_POOL_OFFSET);
    cluster_state    = (volatile cluster_state_t *)(arena + CLUSTER_STATE_OFFSET);

    int nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    g_n_agents = nprocs > 4 ? 4 : nprocs;

    printf("[Arena] %lu MB | CPUs: %d | Agents: %d\n\n",
           (unsigned long)(ARENA_SIZE / 1048576), nprocs, g_n_agents);

    /* ── Launch prophet agents ── */
    pthread_t threads[16];
    int cores[16];

    for (int i = 0; i < g_n_agents; i++) {
        cores[i] = i;
        pthread_create(&threads[i], NULL, prophet_agent, &cores[i]);
    }

    printf("[Main] Running for 30 seconds. Ctrl+C to stop early.\n\n");

    /* ── Run for 30 seconds ── */
    struct timespec ts;
    ts.tv_sec = 30;
    ts.tv_nsec = 0;
    while (keep_running && ts.tv_sec > 0) {
        ts.tv_sec = 1;
        nanosleep(&ts, &ts);
        /* Also print mesh summary every 10 seconds (approximate every 10000 cycles) */
        /* The heavy lifting is in the agents; we just wait here */
    }

    printf("\n[Main] Shutting down...\n");
    keep_running = 0;

    for (int i = 0; i < g_n_agents; i++) {
        pthread_join(threads[i], NULL);
    }

    /* ── Print final mesh summary ── */
    mesh_print_summary();

    /* ── Print final singularity report ── */
    uint32_t total   = __atomic_load_n(&cluster_state->total_predictions, __ATOMIC_RELAXED);
    uint32_t correct = __atomic_load_n(&cluster_state->correct_predictions, __ATOMIC_RELAXED);
    uint32_t n_attr  = __atomic_load_n(&cluster_state->n_attractors, __ATOMIC_RELAXED);
    float acc = total > 0 ? (float)correct / (float)total : 0.0f;

    printf("\n╔══════════════════════════════════════╗\n");
    printf("║   FINAL REPORT                      ║\n");
    printf("╚══════════════════════════════════════╝\n");
    printf("  Agents:          %d\n", g_n_agents);
    printf("  Attractors:      %u\n", n_attr);
    printf("  Rules:           %d\n", n_transition_rules);
    printf("  Training pairs:  %u\n",
           __atomic_load_n(&cluster_state->n_training_pairs, __ATOMIC_RELAXED));
    printf("  Accuracy:        %.3f\n", acc);
    printf("  Correct/Total:   %u/%u\n", correct, total);

    /* ── Cleanup ── */
    munmap((void*)arena, ARENA_SIZE);
    munmap((void*)mesh, sizeof(MeshState));
    shm_unlink(MESH_PATH);

    printf("\n[Done]\n");
    return 0;
}
