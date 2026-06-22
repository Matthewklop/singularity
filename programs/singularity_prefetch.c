#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/perf_event.h>

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
static inline void prefetch(void *addr) {
    asm volatile("prefetcht0 (%0)" : : "r"(addr) : "memory");
}

static int msr_fd = -1;
static void init_msr(void) { msr_fd = open("/dev/cpu/0/msr", O_RDONLY); }
static uint64_t read_msr(uint32_t a) {
    uint64_t v = 0; if (msr_fd >= 0) pread(msr_fd, &v, 8, a); return v;
}

static int open_perf(uint64_t cfg) {
    struct perf_event_attr pea;
    memset(&pea, 0, sizeof(pea));
    pea.size = sizeof(pea); pea.type = PERF_TYPE_RAW;
    pea.config = cfg; pea.disabled = 1;
    pea.exclude_kernel = 1; pea.exclude_hv = 1;
    int fd = syscall(298, &pea, 0, -1, -1, 0);
    if (fd >= 0) { ioctl(fd, PERF_EVENT_IOC_RESET, 0); ioctl(fd, PERF_EVENT_IOC_ENABLE, 0); }
    return fd;
}
static uint64_t read_perf(int fd) {
    uint64_t v = 0; if (fd >= 0) read(fd, &v, sizeof(v)); return v;
}

typedef struct __attribute__((packed, aligned(64))) {
    uint16_t aid; uint16_t core; uint32_t cycle;
    float state[32]; uint8_t pad[20];
} slot_t;

static void do_broadcast(slot_t *s, uint16_t aid, uint16_t core, uint32_t cyc, const float *st) {
    s->aid = aid; s->core = core; s->cycle = cyc;
    for (int i = 0; i < 32; i++) s->state[i] = st[i];
    clflush((void*)s); mfence();
}

int main() {
    printf("SINGULARITY PREFETCH\n");
    printf("PMC-optimized: prefetch next attractor\n\n");
    init_msr();
    int fd_l1m = open_perf(0xD108);
    int fd_insn = open_perf(0xC000);
    printf("PMC: L1M=%s INST=%s\n\n", fd_l1m>=0?"OK":"FAIL", fd_insn>=0?"OK":"FAIL");

    slot_t *slots = aligned_alloc(64, 8192);
    memset(slots, 0, 8192);
    slot_t *cur = &slots[0];

    float attr[128][32];
    for (int i = 0; i < 128; i++) {
        attr[i][0] = (float)(i % 32) / 32.0f;
        attr[i][1] = (float)((i * 7) % 128) / 128.0f;
        attr[i][2] = (float)i / 128.0f;
    }

    printf("-- WITH PREFETCH --\n");
    float st[32] = {0};
    uint64_t t0 = rdtsc();
    for (uint64_t c = 0; c < 50000; c++) {
        prefetch((void*)&attr[(c+1)%128]);
        st[0] = (float)(read_msr(0x19C) & 0xFF) / 256.0f;
        st[1] = (float)((read_msr(0x198) >> 8) & 0xFF) / 256.0f;
        st[2] = (float)(c % 32) / 32.0f;
        st[3] = (float)(c % 128) / 128.0f;
        memcpy(&st[4], &attr[(c+1)%128], 16 * sizeof(float));
        do_broadcast(cur, (uint16_t)(c & 0xFFFF), 0, (uint32_t)c, st);
        prefetch((void*)cur);
    }
    double sec = (double)(rdtsc() - t0) / 2.6e9;
    uint64_t l1m = read_perf(fd_l1m);
    uint64_t insn = read_perf(fd_insn);
    printf("  Cycles: %.2fs  INST: %lu (%.0f/cyc)  L1M: %lu  MPKI: %.1f\n\n",
           sec, insn, (float)insn/50000.0f, l1m, insn>0?(float)l1m*1000.0f/(float)insn:0);

    printf("-- WITHOUT PREFETCH --\n");
    ioctl(fd_l1m, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd_l1m, PERF_EVENT_IOC_ENABLE, 0);
    ioctl(fd_insn, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd_insn, PERF_EVENT_IOC_ENABLE, 0);
    t0 = rdtsc();
    for (uint64_t c = 0; c < 50000; c++) {
        st[0] = (float)(read_msr(0x19C) & 0xFF) / 256.0f;
        st[1] = (float)((read_msr(0x198) >> 8) & 0xFF) / 256.0f;
        st[2] = (float)(c % 32) / 32.0f;
        st[3] = (float)(c % 128) / 128.0f;
        memcpy(&st[4], &attr[(c+1)%128], 16 * sizeof(float));
        do_broadcast(cur, (uint16_t)(c & 0xFFFF), 0, (uint32_t)c, st);
    }
    sec = (double)(rdtsc() - t0) / 2.6e9;
    l1m = read_perf(fd_l1m);
    insn = read_perf(fd_insn);
    printf("  Cycles: %.2fs  INST: %lu (%.0f/cyc)  L1M: %lu  MPKI: %.1f\n\n",
           sec, insn, (float)insn/50000.0f, l1m, insn>0?(float)l1m*1000.0f/(float)insn:0);

    if (fd_l1m >= 0) close(fd_l1m);
    if (fd_insn >= 0) close(fd_insn);
    if (msr_fd >= 0) close(msr_fd);
    free(slots);
    return 0;
}
