// singularity_cuda.cu — GPU-Accelerated Singularity Kernel
// GPU finds nearest attractors for all agents in parallel
// Compile: nvcc -O3 -arch=sm_89 -lpthread -lm -o singularity_cuda singularity_cuda.cu
// Run: timeout 15 ./singularity_cuda

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <math.h>
#include <cuda_runtime.h>
#include <signal.h>

#define STATE_DIM 8
#define MAX_ATTRACTORS 256
#define N_AGENTS 4
#define TRAJECTORY_DEPTH 512
#define MAX_TRAINING_PAIRS 2048

// ─── CUDA kernel: find nearest attractor for each agent ───
// Each block = 1 agent, each thread = 1 attractor
// Output: best_id = closest attractor within radius, 0xFFFF if none
__global__ void find_attractors_kernel(
    const float* __restrict__ states,      // [N_AGENTS * STATE_DIM]
    const float* __restrict__ centers,     // [MAX_ATTRACTORS * STATE_DIM]
    const float* __restrict__ radii,       // [MAX_ATTRACTORS]
    int n_attractors,
    uint16_t* __restrict__ out_ids,        // [N_AGENTS]
    float* __restrict__ out_dists          // [N_AGENTS]
) {
    int agent = blockIdx.x;
    if (agent >= N_AGENTS) return;
    
    // Load agent state into registers
    float s[STATE_DIM];
    for (int i = 0; i < STATE_DIM; i++)
        s[i] = states[agent * STATE_DIM + i];
    
    // Shared memory for per-thread results
    __shared__ float s_min_dist[256];   // max threads = MAX_ATTRACTORS
    __shared__ uint16_t s_min_id[256];
    
    int tid = threadIdx.x;
    s_min_dist[tid] = 1e10f;
    s_min_id[tid] = 0xFFFF;
    
    if (tid < n_attractors) {
        float dist = 0.0f;
        for (int i = 0; i < STATE_DIM; i++) {
            float d = s[i] - centers[tid * STATE_DIM + i];
            dist += d * d;
        }
        s_min_dist[tid] = dist;
        s_min_id[tid] = (uint16_t)tid;
    }
    __syncthreads();
    
    // Parallel reduction to find minimum
    for (int step = 1; step < n_attractors; step <<= 1) {
        if (tid % (step * 2) == 0 && tid + step < n_attractors) {
            if (s_min_dist[tid + step] < s_min_dist[tid]) {
                s_min_dist[tid] = s_min_dist[tid + step];
                s_min_id[tid] = s_min_id[tid + step];
            }
        }
        __syncthreads();
    }
    
    if (tid == 0) {
        float r = radii[s_min_id[0]];
        if (s_min_dist[0] <= r * r) {
            out_ids[agent] = s_min_id[0];
            out_dists[agent] = s_min_dist[0];
        } else {
            out_ids[agent] = 0xFFFF;
            out_dists[agent] = s_min_dist[0] > 1e9f ? 1e9f : s_min_dist[0];
        }
    }
}

// ─── CPU-side structures (from singularity_improved.c) ───
static volatile uint8_t *arena = NULL;
#define ARENA_SIZE (4 * 1024 * 1024)

typedef struct __attribute__((packed)) {
    uint16_t attractor_id; uint16_t source_core; uint32_t cycle;
    float state[STATE_DIM]; uint8_t stability; uint8_t discovering;
    uint8_t pad1; uint32_t reserved; uint8_t padding[17];
} thought_slot_t;

typedef struct __attribute__((packed)) {
    uint16_t id; uint16_t defined_by; uint32_t defined_at_cycle;
    float center[STATE_DIM]; float radius; uint32_t reference_count;
    uint16_t agent_count; uint16_t active; uint8_t generation;
    uint8_t padding[11];
} attractor_def_t;

typedef struct __attribute__((packed)) {
    uint16_t input_sequence[16]; uint16_t next_3[3];
    uint8_t confidence; uint8_t used; uint32_t cycle_recorded;
    uint32_t source_core; uint8_t padding[16];
} training_pair_v2_t;

typedef struct __attribute__((packed)) {
    uint32_t n_attractors; uint32_t n_training_pairs;
    uint32_t global_cycle; uint32_t total_predictions;
    uint32_t correct_predictions; uint8_t padding[44];
} cluster_state_t;

static volatile thought_slot_t *thought_slots = NULL;
static volatile attractor_def_t *attractor_table = NULL;
static volatile training_pair_v2_t *training_pool = NULL;
static volatile cluster_state_t *cluster_state = NULL;

typedef struct {
    uint16_t recent_attractors[TRAJECTORY_DEPTH];
    int head, count;
    uint64_t cycle;
    uint16_t current_attractor, previous_attractor, last_broadcast_id;
    float state[STATE_DIM];
    int core_id;
} agent_local_t;

typedef struct { uint64_t ngram_hash; uint16_t next_attractor; uint16_t count; uint16_t confidence; } transition_rule_t;
#define MAX_RULES 65536
static transition_rule_t rules[MAX_RULES] __attribute__((aligned(64)));
static int n_rules = 0;

typedef struct {
    uint16_t orbit[512]; int orbit_len, orbit_pos;
    int orbit_detected; uint64_t orbit_start_cycle; int agent_id;
} orbit_tracker_t;

// Cache control — portable inline asm for x86
#define CACHE_FLUSH(addr) do { \
    volatile char *_c = (volatile char*)(addr); \
    asm volatile("clflush (%0)" : : "r"(_c) : "memory"); \
} while(0)
#define MEM_FENCE() asm volatile("mfence" : : : "memory")
static inline uint64_t rdtscp(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "ecx");
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t hash_ngram(const uint16_t *seq, int n) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (int i = 0; i < n; i++) { h ^= seq[i]; h *= 0x100000001B3ULL; }
    return h;
}
static int pin_to_core(int c) {
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(c, &cs);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cs);
}

// ─── Agent orbit patterns ───
static const uint8_t agent_orbits[4][32] = {
    {3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3,2,3,8,4,6,2,6,4,3,3,8,3,2,7,9,5},
    {2,7,1,8,2,8,1,8,2,8,4,5,9,0,4,5,2,3,5,3,6,0,2,8,7,4,7,1,3,5,2,6},
    {1,1,2,3,5,8,13,5,2,7,9,0,9,9,8,1,7,6,5,4,3,2,1,0,1,2,3,4,5,6,7,8},
    {1,0,1,0,1,0,1,0,2,1,2,1,2,1,2,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},
};

static void evolve_state(float s[STATE_DIM], uint64_t cycle, int core_id) {
    int idx = core_id % 4;
    int oi = cycle % 32;
    uint8_t base = agent_orbits[idx][oi];
    for (int i = 0; i < STATE_DIM - 1; i++)
        s[i] = (float)base / 16.0f + 0.02f * (float)((base * (i+1) * 3) % 16) / 16.0f;
    s[STATE_DIM - 1] = (float)(cycle % 32) / 32.0f;
}

// ─── Orbit tracking ───
static void track_orbit(orbit_tracker_t *ot, uint16_t aid, uint64_t cycle, uint64_t settle) {
    if (aid == 0xFFFF || cycle < settle) return;
    if (ot->orbit_len < 512) { ot->orbit[ot->orbit_len++] = aid; ot->orbit_pos = ot->orbit_len - 1; return; }
    if (!ot->orbit_detected) {
        for (int p = 16; p <= 256 && p <= 256; p++) {
            if (512 % p) continue;
            int m = 1;
            for (int i = p; i < 512 && m; i++) if (ot->orbit[i] != ot->orbit[i - p]) m = 0;
            if (m) { ot->orbit_len = p; ot->orbit_pos = 0; ot->orbit_detected = 1;
                printf("[Orbit] Agent %d period %d cyc %lu\n", ot->agent_id, p, cycle); return; }
        }
        memmove(ot->orbit, ot->orbit + 256, 256 * 2);
        ot->orbit_len = 256; ot->orbit[ot->orbit_len++] = aid; ot->orbit_pos = ot->orbit_len - 1;
    } else ot->orbit_pos = (ot->orbit_pos + 1) % ot->orbit_len;
}

// ─── CPU-only attractor creation (new attractors) ───
static uint16_t create_attractor_cpu(const float state[STATE_DIM], int core_id, uint64_t cycle) {
    uint32_t nid = __atomic_fetch_add(&cluster_state->n_attractors, 1, __ATOMIC_ACQ_REL);
    if (nid >= MAX_ATTRACTORS) { __atomic_fetch_sub(&cluster_state->n_attractors, 1, __ATOMIC_RELAXED); return 0xFFFF; }
    volatile attractor_def_t *d = &attractor_table[nid];
    d->id = (uint16_t)nid; d->defined_by = (uint16_t)core_id; d->defined_at_cycle = (uint32_t)cycle;
    for (int j = 0; j < STATE_DIM; j++) d->center[j] = state[j];
    d->radius = 0.04f; d->reference_count = 1; d->agent_count = 1; d->active = 1; d->generation = 0;
    CACHE_FLUSH((void*)d);
    return (uint16_t)nid;
}

// ─── Broadcast ───
static void broadcast(int core_id, uint64_t cycle, uint16_t aid, const float s[STATE_DIM]) {
    volatile thought_slot_t *slot = &thought_slots[core_id];
    slot->attractor_id = aid; slot->source_core = (uint16_t)core_id; slot->cycle = (uint32_t)cycle;
    memcpy((void*)slot->state, s, STATE_DIM * sizeof(float));
    slot->stability = aid != 0xFFFF ? 200 : 0; slot->discovering = 0;
    CACHE_FLUSH((void*)slot); MEM_FENCE();
}

// ─── Train ───
static void gen_train(agent_local_t *ag) {
    if (ag->count < 17 || ag->current_attractor == 0xFFFF) return;
    int np = __atomic_load_n(&cluster_state->n_training_pairs, __ATOMIC_RELAXED);
    if (np >= MAX_TRAINING_PAIRS) return;
    uint32_t idx = __atomic_fetch_add(&cluster_state->n_training_pairs, 1, __ATOMIC_ACQ_REL);
    if (idx >= MAX_TRAINING_PAIRS) { __atomic_fetch_sub(&cluster_state->n_training_pairs, 1, __ATOMIC_RELAXED); return; }
    volatile training_pair_v2_t *p = &training_pool[idx];
    for (int i = 0; i < 16; i++) p->input_sequence[i] = ag->recent_attractors[(ag->head - 16 + i + TRAJECTORY_DEPTH) % TRAJECTORY_DEPTH];
    uint16_t n3[3];
    for (int k = 0; k < 3; k++) {
        int si = (ag->head + k) % TRAJECTORY_DEPTH;
        n3[k] = ag->recent_attractors[si];
        if (k == 0 && n3[0] == 0xFFFF) n3[0] = ag->current_attractor;
        if (k > 0 && n3[k] == 0xFFFF) n3[k] = n3[k-1];
    }
    p->next_3[0] = n3[0]; p->next_3[1] = ag->count >= 2 ? n3[1] : n3[0];
    p->next_3[2] = ag->count >= 3 ? n3[2] : n3[1];
    p->confidence = 200; p->used = 0; p->cycle_recorded = (uint32_t)ag->cycle; p->source_core = (uint32_t)ag->core_id;
    CACHE_FLUSH((void*)p);
}

static void learn_pool(void) {
    int np = __atomic_load_n(&cluster_state->n_training_pairs, __ATOMIC_ACQUIRE);
    if (np > MAX_TRAINING_PAIRS) np = MAX_TRAINING_PAIRS;
    for (int i = 0; i < np; i++) {
        volatile training_pair_v2_t *p = &training_pool[i];
        if (p->used || p->next_3[0] == 0xFFFF) continue;
        for (int step = 0; step < 3; step++) {
            uint16_t out = p->next_3[step];
            uint16_t comb[19]; memcpy(comb, (uint16_t*)p->input_sequence, 16*2);
            for (int s = 0; s < step; s++) comb[16 + s] = p->next_3[s];
            int tl = 16 + step;
            for (int n = 2; n <= 16 && n <= tl; n++) {
                uint64_t h = hash_ngram(comb + tl - n, n);
                int found = 0;
                for (int r = 0; r < n_rules && r < MAX_RULES; r++) {
                    if (rules[r].ngram_hash == h) {
                        if (rules[r].next_attractor == out) {
                            if (rules[r].count < 65535) rules[r].count++;
                            rules[r].confidence = (uint16_t)((uint32_t)rules[r].count * 65535 / 4096 > 65535 ? 65535 : (uint32_t)rules[r].count * 65535 / 4096);
                        } else if (rules[r].count > 0) { rules[r].count--;
                            rules[r].confidence = (uint16_t)((uint32_t)rules[r].count * 65535 / 4096 > 65535 ? 65535 : (uint32_t)rules[r].count * 65535 / 4096);
                        }
                        found = 1; break;
                    }
                }
                if (!found && n_rules < MAX_RULES) {
                    int r = __sync_fetch_and_add(&n_rules, 1);
                    if (r < MAX_RULES) { rules[r].ngram_hash = h; rules[r].next_attractor = out; rules[r].count = 1; rules[r].confidence = 16; }
                }
            }
        }
        p->used = 1;
    }
}

static int predict_next(agent_local_t *ag, uint16_t *po, float *co) {
    if (ag->count < 4 || n_rules < 5) { *co = 0.0f; return 0; }
    uint16_t seq[16]; int n = ag->count < 16 ? ag->count : 16;
    for (int i = 0; i < n; i++) seq[i] = ag->recent_attractors[(ag->head - n + i + TRAJECTORY_DEPTH) % TRAJECTORY_DEPTH];
    for (int len = n; len >= 2; len--) {
        uint64_t h = hash_ngram(seq + n - len, len);
        uint16_t best_id = 0xFFFF; int best_cnt = 0, total = 0;
        for (int r = 0; r < n_rules; r++) {
            if (rules[r].ngram_hash == h) { total += rules[r].count; if (rules[r].count > best_cnt) { best_cnt = rules[r].count; best_id = rules[r].next_attractor; } }
        }
        if (total > 0) {
            float conf = (float)best_cnt / (float)total * (float)len / (float)n;
            if (conf > *co && best_cnt >= 5 && best_id != 0xFFFF) { *co = conf; *po = best_id; }
        }
        if (*co > 0.5f) break;
    }
    return *co > 0.2f;
}

static volatile int keep_running = 1;
void handle_sigint(int s) { (void)s; keep_running = 0; }

// ─── GPU state (unified memory) ───
static float *d_states = NULL;
static float *d_centers = NULL;
static float *d_radii = NULL;
static uint16_t *d_out_ids = NULL;
static float *d_out_dists = NULL;
static int gpu_initialized = 0;
static pthread_barrier_t gpu_barrier;

// Per-agent scratch buffers for GPU batch
static __thread float my_state[STATE_DIM];
static __thread uint16_t my_result;
static int batch_cycle = 0;

static int init_gpu(void) {
    cudaSetDevice(0);
    cudaMallocManaged(&d_states, N_AGENTS * STATE_DIM * sizeof(float));
    cudaMallocManaged(&d_centers, MAX_ATTRACTORS * STATE_DIM * sizeof(float));
    cudaMallocManaged(&d_radii, MAX_ATTRACTORS * sizeof(float));
    cudaMallocManaged(&d_out_ids, N_AGENTS * sizeof(uint16_t));
    cudaMallocManaged(&d_out_dists, N_AGENTS * sizeof(float));
    pthread_barrier_init(&gpu_barrier, NULL, N_AGENTS);
    gpu_initialized = 1;
    return 0;
}

// ─── GPU-assisted batch attractor find ───
static void batch_attractor_find(int core_id, const float state[STATE_DIM],
                                  uint16_t *out_id, int *out_created,
                                  uint64_t cycle) {
    if (!gpu_initialized) init_gpu();
    
    // Write my state into the shared buffer
    memcpy(&d_states[core_id * STATE_DIM], state, STATE_DIM * sizeof(float));
    
    // Barrier: wait for all 4 agents to arrive
    pthread_barrier_wait(&gpu_barrier);
    
    // First arrival: launch GPU kernel
    if (core_id == 0) {
        int n = __atomic_load_n(&cluster_state->n_attractors, __ATOMIC_ACQUIRE);
        if (n > 0) {
            // Sync attractor table to unified memory
            for (int i = 0; i < n; i++) {
                memcpy(&d_centers[i * STATE_DIM], (float*)attractor_table[i].center, STATE_DIM * sizeof(float));
                d_radii[i] = attractor_table[i].radius;
            }
            cudaMemPrefetchAsync(d_centers, n * STATE_DIM * sizeof(float), 0, NULL);
            cudaMemPrefetchAsync(d_radii, n * sizeof(float), 0, NULL);
            
            find_attractors_kernel<<<N_AGENTS, 256>>>(d_states, d_centers, d_radii, n, d_out_ids, d_out_dists);
            cudaDeviceSynchronize();
        } else {
            for (int a = 0; a < N_AGENTS; a++) { d_out_ids[a] = 0xFFFF; }
        }
    }
    
    // Second barrier: wait for GPU to complete
    pthread_barrier_wait(&gpu_barrier);
    
    *out_id = d_out_ids[core_id];
    *out_created = (d_out_ids[core_id] == 0xFFFF) ? 1 : 0;
    
    // If no match found, create new attractor on CPU
    if (*out_created) {
        uint32_t nid = __atomic_fetch_add(&cluster_state->n_attractors, 1, __ATOMIC_ACQ_REL);
        if (nid < MAX_ATTRACTORS) {
            volatile attractor_def_t *d = &attractor_table[nid];
            d->id = (uint16_t)nid; d->defined_by = (uint16_t)core_id; d->defined_at_cycle = (uint32_t)cycle;
            for (int j = 0; j < STATE_DIM; j++) d->center[j] = state[j];
            d->radius = 0.04f; d->reference_count = 1; d->agent_count = 1; d->active = 1; d->generation = 0;
            CACHE_FLUSH((void*)d);
            *out_id = (uint16_t)nid;
            *out_created = 0;
        }
    }
}

// ─── Prophet agent ───
void *prophet_agent(void *arg) {
    int core_id = *(int*)arg;
    pin_to_core(core_id);
    agent_local_t ag; memset(&ag, 0, sizeof(ag));
    ag.core_id = core_id; ag.current_attractor = 0xFFFF;
    orbit_tracker_t orb; memset(&orb, 0, sizeof(orb)); orb.agent_id = core_id;
    for (int i = 0; i < STATE_DIM; i++) ag.state[i] = (float)((rdtscp() ^ (core_id << i)) & 0xFF) / 256.0f;
    uint16_t last_pred = 0xFFFF; float last_conf = 0.0f;
    uint64_t last_learn = 0, last_log = 0;
    printf("[Agent %d] ONLINE on core %d\n", core_id, core_id);
    while (keep_running) {
        evolve_state(ag.state, ag.cycle, core_id);
        
        // GPU-batched attractor find (all 4 agents sync via barrier)
        uint16_t aid = 0xFFFF;
        int created = 0;
        batch_attractor_find(core_id, ag.state, &aid, &created, ag.cycle);
        
        if (aid != 0xFFFF) broadcast(core_id, ag.cycle, aid, ag.state);
        
        if (aid != 0xFFFF) {
            ag.previous_attractor = ag.current_attractor;
            ag.current_attractor = aid;
            ag.recent_attractors[ag.head] = aid;
            ag.head = (ag.head + 1) % TRAJECTORY_DEPTH;
            if (ag.count < TRAJECTORY_DEPTH) ag.count++;
            track_orbit(&orb, aid, ag.cycle, 100);
        }
        
        if (last_pred != 0xFFFF && aid != 0xFFFF && ag.cycle % 30 == 1) {
            __atomic_fetch_add(&cluster_state->total_predictions, 1, __ATOMIC_RELAXED);
            if (last_pred == aid) __atomic_fetch_add(&cluster_state->correct_predictions, 1, __ATOMIC_RELAXED);
        }
        
        if (ag.cycle % 20 == 0) gen_train(&ag);
        if (ag.cycle - last_learn > 100) { learn_pool(); last_learn = ag.cycle; }
        
        if (ag.cycle % 30 == 0) {
            uint16_t pred = 0xFFFF; float conf = 0.0f;
            if (orb.orbit_detected && orb.orbit_len > 0) {
                int np = (orb.orbit_pos + 1) % orb.orbit_len;
                pred = orb.orbit[np]; conf = 0.90f;
            } else predict_next(&ag, &pred, &conf);
            last_pred = pred; last_conf = conf;
        }
        
        if (ag.cycle - last_log > 10000) {
            uint32_t t = __atomic_load_n(&cluster_state->total_predictions, __ATOMIC_RELAXED);
            uint32_t c = __atomic_load_n(&cluster_state->correct_predictions, __ATOMIC_RELAXED);
            float acc = t > 0 ? (float)c / (float)t : 0.0f;
            printf("[Agent %d] Cyc %lu | A %d | Pred %d (%.2f) | Acc %.3f (%u/%u) | Attr %u | Rules %d | Orbit %s/%d\n",
                   core_id, ag.cycle, aid, last_pred, last_conf, acc, c, t,
                   __atomic_load_n(&cluster_state->n_attractors, __ATOMIC_ACQUIRE), n_rules,
                   orb.orbit_detected ? "DET" : "SRCH", orb.orbit_len);
            last_log = ag.cycle;
        }
        ag.cycle++;
        for (int j = 0; j < 20; j++) asm volatile("pause" : : : "memory");
    }
    return NULL;
}

// ─── Circuit seed: pre-load truth table as attractors ───
// Each line from the bridge: [SEED] f1 f2 ... fN
static int seed_circuit_attractors(void) {
    char line[256];
    int seeded = 0;
    
    while (fgets(line, sizeof(line), stdin)) {
        if (strncmp(line, "[SEED]", 6) == 0) {
            float vals[STATE_DIM] = {0};
            int n = 0;
            char *p = line + 6;
            while (*p && n < STATE_DIM) {
                while (*p == ' ') p++;
                if (*p < '0' && *p > '9' && *p != '-' && *p != '.') break;
                vals[n++] = strtof(p, &p);
            }
            
            uint32_t nid = __atomic_fetch_add(&cluster_state->n_attractors, 1, __ATOMIC_ACQ_REL);
            if (nid < MAX_ATTRACTORS) {
                volatile attractor_def_t *d = &attractor_table[nid];
                d->id = (uint16_t)nid;
                d->defined_by = 0xFF;  // circuit seed
                d->defined_at_cycle = 0;
                for (int j = 0; j < STATE_DIM; j++) d->center[j] = j < n ? vals[j] : 0.0f;
                d->radius = 0.04f;
                d->reference_count = 1;
                d->agent_count = 0;
                d->active = 1;
                d->generation = 0;
                CACHE_FLUSH((void*)d);
                seeded++;
            }
        }
    }
    return seeded;
}

int main(int argc, char **argv) {
    signal(SIGINT, handle_sigint);
    printf("╔══════════════════════════════════════╗\n");
    printf("║   SINGULARITY KERNEL — CUDA v1.0    ║\n");
    printf("╚══════════════════════════════════════╝\n\n");
    
    int arena_fd = shm_open("/singularity_arena", O_CREAT|O_RDWR, 0666);
    if (arena_fd < 0) { perror("shm_open"); return 1; }
    ftruncate(arena_fd, ARENA_SIZE);
    arena = (volatile uint8_t*)mmap(NULL, ARENA_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, arena_fd, 0);
    if (arena == MAP_FAILED) { perror("mmap"); return 1; }
    memset((void*)arena, 0, ARENA_SIZE);
    close(arena_fd);
    thought_slots = (volatile thought_slot_t*)(arena + 0);
    attractor_table = (volatile attractor_def_t*)(arena + (1<<20));
    training_pool = (volatile training_pair_v2_t*)(arena + (2<<20));
    cluster_state = (volatile cluster_state_t*)(arena + (3<<20));
    
    int nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    int n_agents = nprocs > N_AGENTS ? N_AGENTS : nprocs;
    printf("CPUs: %d | Agents: %d | Arena: 4 MB\n\n", nprocs, n_agents);
    
    // Init GPU
    cudaError_t ce = cudaSetDevice(0);
    if (ce == cudaSuccess) {
        cudaDeviceProp p;
        cudaGetDeviceProperties(&p, 0);
        printf("[GPU] %s CC %d.%d %d SMs %.0f GB\n", p.name, p.major, p.minor, p.multiProcessorCount, p.totalGlobalMem/1e9);
        init_gpu();
    } else {
        printf("[GPU] CUDA unavailable, using CPU only\n");
    }
    
    // Seed circuit truth table as initial attractors
    int n_seeded = seed_circuit_attractors();
    if (n_seeded > 0) {
        printf("[Circuit] Seeded %d attractors from truth table\n", n_seeded);
    } else {
        printf("[Circuit] No seeds provided (pipe in circuit truth table to use seeds)\n");
    }
    
    pthread_t threads[16];
    int cores[16];
    for (int i = 0; i < n_agents; i++) { cores[i] = i; pthread_create(&threads[i], NULL, prophet_agent, &cores[i]); }
    
    printf("[Main] Running. Ctrl+C to stop.\n\n");
    sleep(30);
    
    printf("\n[Main] Shutting down...\n");
    keep_running = 0;
    for (int i = 0; i < n_agents; i++) pthread_join(threads[i], NULL);
    
    uint32_t t = __atomic_load_n(&cluster_state->total_predictions, __ATOMIC_RELAXED);
    uint32_t c = __atomic_load_n(&cluster_state->correct_predictions, __ATOMIC_RELAXED);
    uint32_t na = __atomic_load_n(&cluster_state->n_attractors, __ATOMIC_RELAXED);
    float acc = t > 0 ? (float)c / (float)t : 0.0f;
    
    printf("\n╔══════════════════════════════════════╗\n");
    printf("║   FINAL REPORT                      ║\n");
    printf("╚══════════════════════════════════════╝\n");
    printf("  Agents:          %d\n", n_agents);
    printf("  Attractors:      %u\n", na);
    printf("  Rules:           %d\n", n_rules);
    printf("  Accuracy:        %.3f\n", acc);
    printf("  Correct/Total:   %u/%u\n", c, t);
    printf("  GPU:             %s\n", gpu_initialized ? "initialized" : "not used");
    
    munmap((void*)arena, ARENA_SIZE);
    shm_unlink("/singularity_arena");
    if (d_states) { cudaFree(d_states); cudaFree(d_centers); cudaFree(d_radii); cudaFree(d_out_ids); cudaFree(d_out_dists); }
    printf("\n[Done]\n");
    return 0;
}
