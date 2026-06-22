/*
 * SINGULARITY PREDEBUG — CPU hardware debuggers as mesh sensors.
 * Uses Performance Monitoring Counters (PMCs), Last Branch Records (LBRs),
 * and Intel Processor Trace to observe and optimize the singularity.
 *
 * The CPU debug hardware IS the observability layer.
 * Every pipeline event is a signal in the mesh.
 *
 * Build: gcc -O3 -march=native -o singularity_predebug singularity_predebug.c
 * Run:   sudo ./singularity_predebug  (needs MSR + perf_event access)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <x86intrin.h>

/* ─── CPU direct ─── */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdpmc(uint32_t counter) {
    uint32_t lo, hi;
    asm volatile("rdpmc" : "=a"(lo), "=d"(hi) : "c"(counter));
    return ((uint64_t)hi << 32) | lo;
}

static inline void clflush(void *addr) {
    asm volatile("clflush (%0)" : : "r"(addr) : "memory");
}

static inline void mfence(void) {
    asm volatile("mfence" : : : "memory");
}

/* ─── MSR access ─── */
static int msr_fd = -1;
static void init_msr(void) { msr_fd = open("/dev/cpu/0/msr", O_RDONLY); }
static uint64_t read_msr(uint32_t a) {
    uint64_t v = 0;
    if (msr_fd >= 0) pread(msr_fd, &v, 8, a);
    return v;
}

/* ─── Last Branch Records — read via MSRs ─── */
// LBR records are in MSRs 0x680-0x68F (64-bit: from_ip, to_ip pairs)
#define LBR_FROM_MSR(i) (0x680 + (i) * 2)
#define LBR_TO_MSR(i)   (0x680 + (i) * 2 + 1)
#define LBR_DEPTH 32

typedef struct {
    uint64_t from;
    uint64_t to;
} lbr_entry_t;

static int read_lbr(lbr_entry_t *entries) {
    int count = 0;
    for (int i = 0; i < LBR_DEPTH; i++) {
        uint64_t from = read_msr(LBR_FROM_MSR(i));
        uint64_t to = read_msr(LBR_TO_MSR(i));
        if (from || to) {
            entries[count].from = from;
            entries[count].to = to;
            count++;
        }
    }
    return count;
}

/* ─── Perf events for PMC access ─── */
static int perf_fd = -1;

static int open_perf(uint64_t config, uint64_t umask) {
    struct perf_event_attr pea;
    memset(&pea, 0, sizeof(pea));
    pea.size = sizeof(pea);
    pea.type = PERF_TYPE_RAW;
    pea.config = config | (umask << 8);
    pea.disabled = 1;
    pea.exclude_kernel = 1;
    pea.exclude_hv = 1;
    
    int fd = syscall(298, &pea, 0, -1, -1, 0);
    if (fd >= 0) {
        ioctl(fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    }
    return fd;
}

static uint64_t read_perf(int fd) {
    if (fd < 0) return 0;
    uint64_t val;
    read(fd, &val, sizeof(val));
    return val;
}

/* ─── Thought slot ─── */
typedef struct __attribute__((packed, aligned(64))) {
    uint16_t aid; uint16_t core; uint32_t cycle;
    float state[32]; uint8_t pad[20];
} slot_t;

static void broadcast(slot_t *s, uint16_t aid, uint16_t core, uint32_t cyc, const float *st) {
    s->aid = aid; s->core = core; s->cycle = cyc;
    for (int i = 0; i < 32; i++) s->state[i] = st[i];
    clflush((void*)s); mfence();
}

int main() {
    printf("╔══════════════════════════════════════╗\n");
    printf("║  SINGULARITY PREDEBUG                ║\n");
    printf("║  PMC + LBR + pipeline as mesh sensors║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    init_msr();

    /* ─── Open perf events for key pipeline counters ─── */
    printf("Opening performance counters...\n");
    
    // Event 0xC0: INSTRUCTION_RETIRED
    int fd_insn = open_perf(0xC0, 0x00);
    // Event 0xD1: MEM_LOAD_RETIRED.L1_HIT
    int fd_l1 = open_perf(0xD1, 0x01);
    // Event 0xD1: MEM_LOAD_RETIRED.L1_MISS
    int fd_l1m = open_perf(0xD1, 0x08);
    // Event 0xC4: BR_INST_RETIRED.ALL_BRANCHES
    int fd_br = open_perf(0xC4, 0x00);
    // Event 0xC5: BR_MISP_RETIRED.ALL_BRANCHES
    int fd_brm = open_perf(0xC5, 0x00);
    // Event 0xD0: MEM_UOPS_RETIRED.ALL_STORES
    int fd_st = open_perf(0xD0, 0x08);
    
    printf("  Instructions retired:  %s\n", fd_insn >= 0 ? "OK" : "FAIL");
    printf("  L1 hits:               %s\n", fd_l1 >= 0 ? "OK" : "FAIL");
    printf("  L1 misses:             %s\n", fd_l1m >= 0 ? "OK" : "FAIL");
    printf("  Branches:              %s\n", fd_br >= 0 ? "OK" : "FAIL");
    printf("  Branch mispredicts:    %s\n", fd_brm >= 0 ? "OK" : "FAIL");
    printf("  Stores:                %s\n\n", fd_st >= 0 ? "OK" : "FAIL");

    /* ─── Read LBR once before the loop ─── */
    lbr_entry_t lbr_before[LBR_DEPTH];
    int n_lbr_before = read_lbr(lbr_before);
    printf("LBR entries before: %d\n", n_lbr_before);
    for (int i = 0; i < (n_lbr_before > 4 ? 4 : n_lbr_before); i++)
        printf("  [%02d] 0x%016lX → 0x%016lX\n", i, lbr_before[i].from, lbr_before[i].to);

    /* ─── Allocate slot ─── */
    slot_t *slot = aligned_alloc(64, 4096);
    memset(slot, 0, 4096);

    printf("\n── Singularity loop with pipeline monitoring ──\n\n");

    float state[32] = {0};
    uint64_t t0 = rdtsc();

    for (uint64_t c = 0; c < 50000; c++) {
        /* Evolve attractor state */
        state[0] = (float)(read_msr(0x19C) & 0xFF) / 256.0f;
        state[1] = (float)((read_msr(0x198) >> 8) & 0xFF) / 256.0f;
        state[2] = (float)(c % 32) / 32.0f;
        state[3] = (float)((read_msr(0x1FC) >> 8) & 0xFF) / 256.0f;
        state[4] = (float)(c % 128) / 128.0f;  /* wider orbit */
        
        broadcast(slot, (uint16_t)(c & 0xFFFF), 0, (uint32_t)c, state);
        
        /* Every 5000 cycles, read pipeline stats */
        if (c > 0 && c % 5000 == 0) {
            uint64_t insn = read_perf(fd_insn);
            uint64_t l1h = read_perf(fd_l1);
            uint64_t l1m = read_perf(fd_l1m);
            uint64_t br = read_perf(fd_br);
            uint64_t brm = read_perf(fd_brm);
            uint64_t st = read_perf(fd_st);
            
            float mpki = (float)l1m * 1000.0f / (float)(insn ? insn : 1);
            float bmr = br > 0 ? (float)brm / (float)br * 100.0f : 0;
            
            printf("[%05lu] insn=%lu l1_h=%lu l1_m=%lu MPKI=%.1f br=%lu br_misp=%.1f%% stores=%lu\n",
                   c, insn, l1h, l1m, mpki, br, bmr, st);
        }
        
        for (int i = 0; i < 30; i++) _mm_pause();
    }

    double sec = (double)(rdtsc() - t0) / 2.6e9;
    
    /* ─── Read LBR after loop ─── */
    lbr_entry_t lbr_after[LBR_DEPTH];
    int n_lbr_after = read_lbr(lbr_after);
    printf("\n── LBR after loop (%d entries) ──\n", n_lbr_after);
    for (int i = 0; i < (n_lbr_after > 8 ? 8 : n_lbr_after); i++)
        printf("  [%02d] 0x%016lX → 0x%016lX\n", i, lbr_after[i].from, lbr_after[i].to);

    /* ─── Final perf summary ─── */
    uint64_t fin_insn = read_perf(fd_insn);
    uint64_t fin_l1h = read_perf(fd_l1);
    uint64_t fin_l1m = read_perf(fd_l1m);
    printf("\n[Done] 50K cycles in %.2fs\n", sec);
    printf("  Total instructions: %lu (%.0f / cycle)\n", fin_insn, (float)fin_insn / 50000.0f);
    printf("  L1 hit rate: %.1f%%\n", fin_insn > 0 ? (float)fin_l1h / (float)fin_insn * 100.0f : 0);
    printf("  MPKI: %.1f\n", fin_insn > 0 ? (float)fin_l1m * 1000.0f / (float)fin_insn : 0);

    /* Cleanup */
    if (fd_insn >= 0) close(fd_insn);
    if (fd_l1 >= 0) close(fd_l1);
    if (fd_l1m >= 0) close(fd_l1m);
    if (fd_br >= 0) close(fd_br);
    if (fd_brm >= 0) close(fd_brm);
    if (fd_st >= 0) close(fd_st);
    if (msr_fd >= 0) close(msr_fd);
    free(slot);
    return 0;
}
