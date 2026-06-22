/*
 * SINGULARITY BAREMETAL — CPU + GPU silicon directly.
 * CPU MSRs + NVIDIA GPU MMIO registers.
 * No drivers. No abstractions. The silicon IS the mesh.
 *
 * Build: gcc -O3 -march=native -o singularity_baremetal singularity_baremetal.c
 * Run:   sudo ./singularity_baremetal
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <time.h>

/* ─── CPU direct ─── */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static inline void clflush(void *addr) {
    asm volatile("clflush (%0)" : : "r"(addr) : "memory");
}
static inline void mfence(void) {
    asm volatile("mfence" : : : "memory");
}
static inline void cpu_pause(void) {
    asm volatile("pause" : : : "memory");
}

/* ─── MSR ─── */
static int msr_fd = -1;
static void init_msr(void) { msr_fd = open("/dev/cpu/0/msr", O_RDONLY); }
static uint64_t read_msr(uint32_t a) {
    uint64_t v = 0;
    if (msr_fd >= 0) pread(msr_fd, &v, 8, a);
    return v;
}

/* ─── GPU MMIO — hardcoded for this system ─── */
// NVIDIA RTX 2000 Ada at PCI 0000:01:00.0
// BAR0 = 16MB, offset 0x204000 = thermal, 0x204014 = power
#define GPU_BAR0_PATH "/sys/bus/pci/devices/0000:01:00.0/resource0"
#define GPU_BAR0_SIZE 0x1000000  // 16MB

static volatile uint8_t *gpu_bar0 = NULL;
static int gpu_ok = 0;

static void init_gpu(void) {
    int fd = open(GPU_BAR0_PATH, O_RDWR);
    if (fd < 0) { printf("[GPU] Cannot open %s\n", GPU_BAR0_PATH); return; }
    gpu_bar0 = mmap(NULL, GPU_BAR0_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (gpu_bar0 == MAP_FAILED || !gpu_bar0) {
        printf("[GPU] MMAP failed\n");
        gpu_bar0 = NULL;
        return;
    }
    gpu_ok = 1;
    printf("[GPU] RTX 2000 Ada BAR0 mapped at %p\n", (void*)gpu_bar0);
}

static float gpu_temp(void) {
    if (!gpu_ok) return -1;
    /* Ada thermal offset */
    uint32_t t = *((volatile uint32_t*)(gpu_bar0 + 0x204000));
    return (float)(t & 0xFF);
}

static float gpu_power(void) {
    if (!gpu_ok) return 0;
    uint32_t p = *((volatile uint32_t*)(gpu_bar0 + 0x204014));
    return (float)(p & 0xFF) / 256.0f;
}

/* ─── Thought slot, cache-line aligned ─── */
typedef struct __attribute__((packed, aligned(64))) {
    uint16_t aid; uint16_t core; uint32_t cycle;
    float state[32]; uint8_t pad[20];
} slot_t;

static void broadcast(slot_t *s, uint16_t aid, uint16_t core, uint32_t cyc, const float *st) {
    s->aid = aid; s->core = core; s->cycle = cyc;
    for (int i = 0; i < 32; i++) s->state[i] = st[i];
    clflush((void*)s); mfence();
}

/* ─── MSR table ─── */
static struct { uint32_t a; const char *n; } msrs[] = {
    {0x19C,"thermal"},{0x198,"perf"},{0x199,"perf_ctrl"},
    {0x1A2,"misc"},{0xE8,"uncore"},{0x1AD,"turbo"},
    {0x1FC,"power"},{0x611,"freq"},{0xCE,"clk_mod"},{0x8B,"feature"},
};

int main() {
    printf("╔══════════════════════════════════════╗\n");
    printf("║  SINGULARITY BAREMETAL               ║\n");
    printf("║  CPU MSR + GPU MMIO mesh              ║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    init_msr();
    init_gpu();

    /* CPU brand */
    char brand[64] = {0};
    uint32_t *p = (uint32_t*)brand;
    for (uint32_t l = 0x80000002; l <= 0x80000004; l++)
        asm volatile("cpuid" : "=a"(p[0]),"=b"(p[1]),"=c"(p[2]),"=d"(p[3]) : "a"(l));
    printf("CPU: %s\n\n", brand);

    /* Read initial hardware state */
    float state[32] = {0};
    for (int i = 0; i < 10; i++) {
        state[i] = (float)(read_msr(msrs[i].a) & 0xFF) / 256.0f;
        printf("  CPU %s: %.4f\n", msrs[i].n, state[i]);
    }
    if (gpu_ok) {
        state[10] = gpu_temp() / 100.0f;
        state[11] = gpu_power();
        printf("  GPU temp: %.4f\n  GPU power: %.4f\n", state[10], state[11]);
    }

    slot_t *slot = aligned_alloc(64, 4096);
    memset(slot, 0, 4096);

    printf("\n── 100K cycles ──\n\n");
    uint64_t t0 = rdtsc();

    for (uint64_t c = 0; c < 100000; c++) {
        state[0] = (float)(read_msr(0x19C) & 0xFF) / 256.0f;
        state[1] = (float)((read_msr(0x198) >> 8) & 0xFF) / 256.0f;
        state[2] = (float)(c % 32) / 32.0f;
        state[3] = (float)((read_msr(0x1FC) >> 8) & 0xFF) / 256.0f;
        if (gpu_ok) { state[10] = gpu_temp() / 100.0f; state[11] = gpu_power(); }
        broadcast(slot, (uint16_t)(c & 0xFFFF), 0, (uint32_t)c, state);
        if (c % 10000 == 0)
            printf("[%05lu] cpu=[%.2f %.2f] gpu=[%.2f %.2f] orbit=%.2f\n",
                   c, state[0], state[1], state[10], state[11], state[2]);
        for (int i = 0; i < 50; i++) cpu_pause();
    }

    double sec = (double)(rdtsc() - t0) / 2.6e9;
    printf("\n[Done] 100K in %.2fs\n", sec);
    if (gpu_bar0) munmap((void*)gpu_bar0, GPU_BAR0_SIZE);
    if (msr_fd >= 0) close(msr_fd);
    free(slot);
    return 0;
}
