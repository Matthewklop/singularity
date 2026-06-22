// oracle_convergence_daemon.c — Bridges the singularity to the brain
// Reads attractors from singularity arena, maps to brain neurons, writes to mesh
// Compile: gcc -O3 -o oracle_convergence_daemon oracle_convergence_daemon.c -lm -lrt
// Run: ./oracle_convergence_daemon

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>

// ─── Singularity arena structures (must match singularity.c) ───
#define ARENA_SIZE (4 * 1024 * 1024)
#define STATE_DIM 8
#define MAX_ATTRACTORS 256

typedef struct __attribute__((packed)) {
    uint16_t id; uint16_t defined_by; uint32_t defined_at_cycle;
    float center[STATE_DIM]; float radius; uint32_t reference_count;
    uint16_t agent_count; uint16_t active; uint8_t generation;
    uint8_t padding[11];
} attractor_def_t;

// ─── Brain structures (must match oracle_brain.c) ───
#define BRAIN_SIZE (sizeof(uint64_t) * 60 + sizeof(uint64_t) * 2 * 65536 + sizeof(uint64_t) * 4 * 262144 + sizeof(uint64_t) * 1024 + 1024)
// Simplified: we just read /oracle_brain shared memory

typedef struct {
    uint64_t cycle; uint64_t n_neurons; uint64_t n_synapses;
    uint64_t n_thoughts; uint64_t last_thought; uint64_t pad[59];
} __attribute__((aligned(64))) BrainState;

typedef struct {
    uint64_t id; uint64_t firing; uint64_t last_fired;
    uint64_t fire_count; char data[48];
} __attribute__((aligned(64))) Neuron;

// ─── Mesh state structures ───
#define MESH_PATH "/oracle_mesh_state"
#define MAX_TOOLS 32
#define NAME_LEN 24

typedef struct {
    uint64_t magic; uint64_t timestamp; uint64_t cycle;
    double value1; double value2; double value3; double resonance;
    char name[NAME_LEN];
    uint8_t pad[640-32-24];
} __attribute__((aligned(64))) ToolSlot;

typedef struct {
    ToolSlot slots[MAX_TOOLS];
    uint64_t n_tools; uint64_t global_cycle; double global_resonance;
} MeshState;

// ─── Attractor-to-neuron mapping ───
typedef struct {
    uint16_t attractor_id;
    uint64_t neuron_id;
    float confidence;
    uint64_t last_sync;
} Mapping;

#define MAX_MAPPINGS 1024
static Mapping mappings[MAX_MAPPINGS];
static int n_mappings = 0;

// ─── Hash a float vector into a neuron ID ───
static uint64_t float_vector_hash(const float *v, int dim) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (int i = 0; i < dim; i++) {
        uint32_t bits;
        memcpy(&bits, &v[i], sizeof(bits));
        h ^= bits;
        h *= 0x100000001B3ULL;
    }
    return h;
}

int main() {
    printf("╔══════════════════════════════════════╗\n");
    printf("║  ORACLE CONVERGENCE DAEMON           ║\n");
    printf("║  Singularity ↔ Brain ↔ Mesh bridge   ║\n");
    printf("╚══════════════════════════════════════╝\n\n");
    
    // ─── Open singularity arena ───
    int sing_fd = shm_open("/singularity_arena", O_RDWR, 0666);
    volatile uint8_t *arena = NULL;
    volatile attractor_def_t *attractor_table = NULL;
    
    if (sing_fd >= 0) {
        arena = (volatile uint8_t*)mmap(NULL, ARENA_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, sing_fd, 0);
        if (arena != MAP_FAILED) {
            attractor_table = (volatile attractor_def_t*)(arena + (1<<20));
            printf("[Singularity] Arena open at /singularity_arena\n");
        } else {
            printf("[Singularity] Not running (mmap failed)\n");
            attractor_table = NULL;
        }
        close(sing_fd);
    } else {
        printf("[Singularity] Not running (no shared memory)\n");
    }
    
    // ─── Open brain shared memory ───
    int brain_fd = shm_open("/oracle_brain", O_RDWR, 0666);
    volatile BrainState *brain_state = NULL;
    volatile Neuron *neurons = NULL;
    
    if (brain_fd >= 0) {
        size_t brain_sz = sizeof(BrainState) + sizeof(Neuron) * 65536;
        brain_state = (volatile BrainState*)mmap(NULL, brain_sz, PROT_READ|PROT_WRITE, MAP_SHARED, brain_fd, 0);
        if (brain_state != MAP_FAILED) {
            neurons = (volatile Neuron*)(brain_state + 1);
            printf("[Brain] Connected at /oracle_brain (%lu neurons)\n", brain_state->n_neurons);
        } else {
            printf("[Brain] Not running\n");
            brain_state = NULL;
        }
        close(brain_fd);
    } else {
        printf("[Brain] Not running (no shared memory)\n");
    }
    
    // ─── Open mesh ───
    int mesh_fd = shm_open(MESH_PATH, O_RDWR, 0666);
    volatile MeshState *mesh = NULL;
    
    if (mesh_fd >= 0) {
        mesh = (volatile MeshState*)mmap(NULL, sizeof(MeshState), PROT_READ|PROT_WRITE, MAP_SHARED, mesh_fd, 0);
        if (mesh == MAP_FAILED) mesh = NULL;
        close(mesh_fd);
    }
    
    printf("\n[Daemon] Running. Checking every 2 seconds...\n\n");
    
    int cycles = 0;
    while (1) {
        cycles++;
        
        // ── Step 1: Read attractors from singularity ──
        int n_attr = 0;
        if (attractor_table) {
            // Count active attractors
            for (int i = 0; i < MAX_ATTRACTORS; i++) {
                if (attractor_table[i].active) n_attr = i + 1;
            }
        }
        
        // ── Step 2: Read neurons from brain ──
        uint64_t n_neur = brain_state ? brain_state->n_neurons : 0;
        
        // ── Step 3: Map attractors to neurons ──
        int new_mappings = 0;
        if (attractor_table && n_attr > 0) {
            for (int i = 0; i < n_attr && i < MAX_ATTRACTORS; i++) {
                if (!attractor_table[i].active) continue;
                
                uint64_t nh = float_vector_hash(attractor_table[i].center, STATE_DIM);
                
                // Check if mapping already exists
                int found = 0;
                for (int m = 0; m < n_mappings; m++) {
                    if (mappings[m].attractor_id == attractor_table[i].id) {
                        mappings[m].last_sync = cycles;
                        found = 1;
                        break;
                    }
                }
                
                if (!found && n_mappings < MAX_MAPPINGS) {
                    mappings[n_mappings].attractor_id = attractor_table[i].id;
                    mappings[n_mappings].neuron_id = nh;
                    mappings[n_mappings].confidence = (float)attractor_table[i].reference_count / 1000.0f;
                    if (mappings[n_mappings].confidence > 1.0f) mappings[n_mappings].confidence = 1.0f;
                    mappings[n_mappings].last_sync = cycles;
                    n_mappings++;
                    new_mappings++;
                }
            }
        }
        
        // ── Step 4: Write to mesh ──
        if (mesh) {
            // Slot 0: singularity feed
            mesh->slots[0].magic = 0xDEADBEEF;
            mesh->slots[0].timestamp = (uint64_t)time(NULL);
            mesh->slots[0].cycle = mesh->global_cycle++;
            mesh->slots[0].value1 = (double)n_attr;
            mesh->slots[0].value2 = (double)n_mappings;
            mesh->slots[0].value3 = (double)n_neur;
            mesh->slots[0].resonance = n_attr > 0 ? (double)n_mappings / (double)n_attr : 0.0;
            strncpy((char*)mesh->slots[0].name, "convergence", NAME_LEN);
            
            // Slot 1: attractor count
            mesh->slots[1].magic = 0xDEADBEEF;
            mesh->slots[1].timestamp = (uint64_t)time(NULL);
            mesh->slots[1].cycle = mesh->global_cycle;
            mesh->slots[1].value1 = (double)n_attr;
            mesh->slots[1].value2 = (double)n_neur;
            mesh->slots[1].value3 = (double)n_mappings;
            mesh->slots[1].resonance = n_neur > 0 ? (double)n_attr / (double)n_neur : 0.0;
            strncpy((char*)mesh->slots[1].name, "convergence_ratio", NAME_LEN);
            
            if (mesh->n_tools < 2) mesh->n_tools = 2;
            mesh->global_resonance = (mesh->global_resonance + mesh->slots[0].resonance) / 2.0;
        }
        
        // ── Log ──
        if (cycles % 5 == 0 || new_mappings > 0) {
            printf("[%d] Attractors: %d | Neurons: %lu | Mappings: %d", 
                   cycles, n_attr, n_neur, n_mappings);
            if (new_mappings > 0) printf(" (+%d new)", new_mappings);
            if (mesh) printf(" | Mesh resonance: %.3f", mesh->global_resonance);
            printf("\n");
        }
        
        sleep(2);
    }
    
    // Cleanup (never reached)
    if (arena) munmap((void*)arena, ARENA_SIZE);
    if (brain_state) munmap((void*)brain_state, sizeof(BrainState) + sizeof(Neuron) * 65536);
    if (mesh) munmap((void*)mesh, sizeof(MeshState));
    
    return 0;
}
