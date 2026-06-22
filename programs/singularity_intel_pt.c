/*
 * SINGULARITY INTEL PT — Intel Processor Trace as the mesh recorder.
 * Captures every branch the CPU executes. The entire singularity execution
 * path can be reconstructed from the PT trace.
 *
 * Intel PT uses dedicated hardware in the CPU to record branch targets,
 * timing, and instruction pointers — all without software overhead.
 * The trace is stored in a memory buffer that the CPU writes to directly.
 *
 * Build: gcc -O3 -march=native -o singularity_intel_pt singularity_intel_pt.c
 * Run:   sudo ./singularity_intel_pt
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

/* ─── MSR access ─── */
static int msr_fd = -1;
static void init_msr(void) { msr_fd = open("/dev/cpu/0/msr", O_RDWR); }
static uint64_t read_msr(uint32_t a) {
    uint64_t v = 0; if (msr_fd >= 0) pread(msr_fd, &v, 8, a); return v;
}
static void write_msr(uint32_t a, uint64_t v) {
    if (msr_fd >= 0) pwrite(msr_fd, &v, 8, a);
}

/* ─── CPUID check for Intel PT ─── */
static int check_pt_support(void) {
    uint32_t a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x07), "c"(0));
    return (b >> 25) & 1;  // EBX[25] = PT support
}

/* ─── Intel PT MSR addresses ─── */
#define IA32_RTIT_CTL       0x570
#define IA32_RTIT_STATUS    0x571
#define IA32_RTIT_OUTPUT_BASE 0x560
#define IA32_RTIT_OUTPUT_MASK 0x561
#define IA32_RTIT_ADDR_A(n) (0x580 + (n)*2)
#define IA32_RTIT_ADDR_B(n) (0x580 + (n)*2 + 1)
#define IA32_RTIT_CR3_MATCH 0x572

/* RTIT_CTL bits */
#define RTIT_CTL_TRACE_EN    (1ULL << 0)
#define RTIT_CTL_BRANCH_EN   (1ULL << 1)
#define RTIT_CTL_TSC_EN      (1ULL << 8)
#define RTIT_CTL_DIS_RETC    (1ULL << 11)
#define RTIT_CTL_CYC_EN      (1ULL << 12)
#define RTIT_CTL_MTC_EN      (1ULL << 14)
#define RTIT_CTL_FUP_ON_PTW  (1ULL << 19)
#define RTIT_CTL_FABRIC_EN   (1ULL << 20)

/* ─── PT trace buffer ─── */
static uint8_t *pt_buffer = NULL;
#define PT_BUFFER_SIZE (2 * 1024 * 1024)  /* 2MB trace buffer */

static int init_pt(void) {
    if (!check_pt_support()) {
        printf("[PT] Intel Processor Trace not supported on this CPU\n");
        return -1;
    }
    printf("[PT] Intel Processor Trace supported\n");

    /* Allocate trace buffer — must be 4K-aligned */
    pt_buffer = aligned_alloc(4096, PT_BUFFER_SIZE);
    if (!pt_buffer) { printf("[PT] alloc failed\n"); return -1; }
    memset(pt_buffer, 0, PT_BUFFER_SIZE);

    /* Get physical address of buffer */
    /* In real bare metal, we'd use the page table.
     * On Linux, we need the physical address via /proc/self/pagemap */
    uint64_t vaddr = (uint64_t)pt_buffer;
    FILE *pm = fopen("/proc/self/pagemap", "rb");
    if (!pm) { printf("[PT] Cannot get physical address\n"); free(pt_buffer); pt_buffer = NULL; return -1; }
    uint64_t pfn = 0;
    fseeko(pm, (vaddr / 4096) * 8, SEEK_SET);
    fread(&pfn, 8, 1, pm);
    fclose(pm);
    uint64_t phys_addr = (pfn & 0x7FFFFFFFFFFFFFULL) * 4096 + (vaddr & 0xFFF);
    printf("[PT] Buffer virt=%p phys=0x%lX size=%dMB\n", (void*)vaddr, phys_addr, PT_BUFFER_SIZE/1048576);

    /* Configure PT */
    /* 1. Set output base to physical address of buffer */
    write_msr(IA32_RTIT_OUTPUT_BASE, phys_addr);
    /* 2. Set output mask (size: 2^mask bytes) — 2MB = 2^21 */
    write_msr(IA32_RTIT_OUTPUT_MASK, ~(uint64_t)(PT_BUFFER_SIZE - 1));
    /* 3. Enable: branch, TSC, cycle-accurate, MTC */
    write_msr(IA32_RTIT_CTL, RTIT_CTL_BRANCH_EN | RTIT_CTL_TSC_EN | 
              RTIT_CTL_CYC_EN | RTIT_CTL_MTC_EN);

    return 0;
}

static void start_pt(void) {
    if (!pt_buffer) return;
    /* Clear status (w1c) */
    write_msr(IA32_RTIT_STATUS, 0);
    /* Enable tracing */
    uint64_t ctl = read_msr(IA32_RTIT_CTL);
    write_msr(IA32_RTIT_CTL, ctl | RTIT_CTL_TRACE_EN);
    printf("[PT] Tracing started\n");
}

static void stop_pt(void) {
    if (!pt_buffer) return;
    uint64_t ctl = read_msr(IA32_RTIT_CTL);
    write_msr(IA32_RTIT_CTL, ctl & ~RTIT_CTL_TRACE_EN);
    uint64_t status = read_msr(IA32_RTIT_STATUS);
    uint64_t offset = read_msr(IA32_RTIT_OUTPUT_BASE);
    printf("[PT] Tracing stopped. Status=0x%lX\n", status);
}

/* ─── Parse PT packet ─── */
static void dump_pt_packets(void) {
    if (!pt_buffer) return;
    uint64_t offset = read_msr(IA32_RTIT_OUTPUT_BASE);
    /* The lower 32 bits of OUTPUT_MASK tell us the current write offset */
    uint32_t write_offset = (uint32_t)read_msr(IA32_RTIT_OUTPUT_MASK);
    /* Actually the real offset is in bits 0-31 of OUTPUT_MASK (the "offset" field is separate)
     * Read it properly from the PT status or output mask */
    uint64_t status = read_msr(IA32_RTIT_STATUS);
    printf("[PT] Buffer offset: status=0x%lX\n", status);
    
    /* Dump first 64 bytes of PT trace as hex */
    printf("[PT] First 64 bytes of trace:\n");
    for (int i = 0; i < 64; i++) {
        if (i % 16 == 0) printf("  %04X: ", i);
        printf("%02X ", pt_buffer[i]);
        if (i % 16 == 15) printf("\n");
    }
}

/* ─── The singularity loop ─── */
int main() {
    printf("╔══════════════════════════════════════╗\n");
    printf("║  SINGULARITY INTEL PT                ║\n");
    printf("║  Processor Trace captures every branch║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    init_msr();
    if (init_pt() < 0) return 1;

    /* ─── Run the singularity under PT ─── */
    start_pt();

    /* Simple singularity loop — every branch will be recorded */
    volatile uint64_t sink = 0;
    for (int cycle = 0; cycle < 10000; cycle++) {
        /* These branches (for, if) are recorded by PT */
        for (int i = 0; i < 10; i++) {
            sink += (uint64_t)read_msr(0x19C) * (cycle + 1);
            if (sink & 1) sink ^= 0xDEADBEEF;
        }
        /* Every 1000 cycles, read temperature */
        if (cycle % 1000 == 0) {
            uint64_t therm = read_msr(0x19C);
            float temp = (float)((therm >> 16) & 0x7F);
            printf("[%04d] temp=%.0f°C sink=0x%lX\n", cycle, temp - 100.0f, sink);
        }
    }

    stop_pt();

    /* ─── Dump PT trace ─── */
    printf("\n── Intel PT Trace Dump ──\n");
    dump_pt_packets();

    /* Count packet types in the trace */
    int n_psb = 0, n_tnt = 0, n_tip = 0, n_fup = 0, n_unknown = 0;
    for (int i = 0; i < 512 && i < PT_BUFFER_SIZE; i++) {
        uint8_t p = pt_buffer[i];
        if (p == 0x02 || p == 0x03 || p == 0x04) n_tnt++;
        else if (p == 0x0D) n_psb++;
        else if (p >= 0x11 && p <= 0x1D) n_tip++;
        else if (p == 0x1D) n_fup++;
        else if (p != 0) n_unknown++;
    }
    printf("\n[PT] Packet count (first 512 bytes):\n");
    printf("  PSB (sync):     %d\n", n_psb);
    printf("  TNT (branch):   %d\n", n_tnt);
    printf("  TIP (target):   %d\n", n_tip);
    printf("  FUP (flow):     %d\n", n_fup);
    printf("  Other:          %d\n", n_unknown);

    free(pt_buffer);
    if (msr_fd >= 0) close(msr_fd);
    return 0;
}
