/* ============================================================================
 * oracle_mesh_state.c — Unified shared memory state for all oracle tools
 *
 * Every oracle tool writes its state here. The mesh singularity reads it.
 * Standardized format. One header file. All tools share it.
 *
 * This is both the writer (each tool updates its slot) and the reader
 * (the singularity reads all slots and converges them).
 *
 * Build: gcc -O3 -o oracle_mesh_state oracle_mesh_state.c -lm -lrt
 * Run:   ./oracle_mesh_state [tool_name] [value1] [value2] [value3]
 *
 * Without args: display current mesh state
 * With args: update a tool's state
 * ============================================================================
 */
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
#include <stddef.h>

#define P "/oracle_mesh_state"
#define MAX_TOOLS 32
#define NAME_LEN 24

typedef struct {
    uint64_t magic;
    uint64_t timestamp;
    uint64_t cycle;
    double value1;     /* primary metric */
    double value2;     /* secondary metric */
    double value3;     /* tertiary metric */
    double resonance;  /* self-reported coherence */
    char name[NAME_LEN];
    uint8_t pad[640-32-24];
} __attribute__((aligned(64))) ToolSlot;

typedef struct {
    ToolSlot slots[MAX_TOOLS];
    uint64_t n_tools;
    uint64_t global_cycle;
    double global_resonance;
} MeshState;

static uint64_t now_ns(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}

/* ─── Register or update a tool's slot ─── */
static int update_tool(MeshState *m, const char *name, double v1, double v2, double v3, double res) {
    /* Find existing slot */
    for(uint64_t i=0;i<m->n_tools;i++){
        if(strcmp(m->slots[i].name, name)==0){
            m->slots[i].timestamp=now_ns();
            m->slots[i].cycle++;
            m->slots[i].value1=v1;
            m->slots[i].value2=v2;
            m->slots[i].value3=v3;
            m->slots[i].resonance=res;
            return i;
        }
    }
    /* Create new slot */
    if(m->n_tools>=MAX_TOOLS) return -1;
    int i=m->n_tools++;
    memset(&m->slots[i],0,sizeof(ToolSlot));
    m->slots[i].magic=0x4D455348;
    m->slots[i].timestamp=now_ns();
    m->slots[i].cycle=1;
    m->slots[i].value1=v1;
    m->slots[i].value2=v2;
    m->slots[i].value3=v3;
    m->slots[i].resonance=res;
    strncpy(m->slots[i].name, name, NAME_LEN-1);
    return i;
}

/* ─── Find a tool by name ─── */
static ToolSlot *find_tool(MeshState *m, const char *name) {
    for(uint64_t i=0;i<m->n_tools;i++)
        if(strcmp(m->slots[i].name, name)==0) return &m->slots[i];
    return NULL;
}

/* ─── Compute global resonance across all tools ─── */
static void compute_resonance(MeshState *m) {
    double sum=0; int n=0;
    uint64_t now=now_ns();
    for(uint64_t i=0;i<m->n_tools;i++){
        /* Only count tools updated in the last 60 seconds */
        if(now - m->slots[i].timestamp < 60000000000ULL){
            sum += m->slots[i].resonance;
            n++;
        }
    }
    m->global_resonance = n>0 ? sum/n : 0.5;
    m->global_cycle++;
}

int main(int argc, char **argv) {
    int fd=shm_open(P,O_CREAT|O_RDWR,0644);
    if(fd<0){perror("shm_open");return 1;}
    ftruncate(fd,sizeof(MeshState));
    MeshState*m=mmap(0,sizeof(MeshState),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(m==MAP_FAILED){perror("mmap");return 1;}

    if(m->n_tools==0) m->n_tools=0;

    if(argc>1){
        /* Update mode: ./oracle_mesh_state toolname v1 v2 v3 resonance */
        const char *name=argv[1];
        double v1=argc>2?atof(argv[2]):0.5;
        double v2=argc>3?atof(argv[3]):0.5;
        double v3=argc>4?atof(argv[4]):0.5;
        double res=argc>5?atof(argv[5]):(v1+v2+v3)/3.0;
        update_tool(m,name,v1,v2,v3,res);
        compute_resonance(m);
        munmap(m,sizeof(MeshState));
        return 0;
    }

    /* Display mode */
    printf("╔══════════════════════════════════════╗\n");
    printf("║  MESH STATE — All Oracle Tools      ║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    compute_resonance(m);
    printf("  Global cycle: %lu\n", (unsigned long)m->global_cycle);
    printf("  Global resonance: %.4f\n", m->global_resonance);
    printf("  Active tools: %lu\n\n", (unsigned long)m->n_tools);
    printf("  %-22s %12s %12s %12s %12s %12s\n", "Tool", "V1", "V2", "V3", "Resonance", "Age");
    printf("  %s\n", "─────────────────────────────────────────────────────────────────────────────");

    uint64_t now=now_ns();
    for(uint64_t i=0;i<m->n_tools;i++){
        ToolSlot *ts=&m->slots[i];
        uint64_t age_ms=(now-ts->timestamp)/1000000;
        const char *status = age_ms<5000 ? "●" : (age_ms<60000 ? "◐" : "○");
        printf("  %s%-22s %12.4f %12.4f %12.4f %12.4f %4lus\n",
               status, ts->name,
               ts->value1, ts->value2, ts->value3,
               ts->resonance,
               (unsigned long)(age_ms/1000));
    }

    printf("\n  ● = active (<5s)  ◐ = recent (<60s)  ○ = stale\n\n");
    printf("  To update: ./oracle_mesh_state <tool> <v1> <v2> <v3> [resonance]\n\n");

    munmap(m,sizeof(MeshState));
    return 0;
}
