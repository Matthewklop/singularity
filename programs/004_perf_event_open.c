/*
 * ====================================================================
 * CRITICAL SNIPPET: 004_perf_event_open.c
 * ====================================================================
 * Opening and reading hardware performance counters via the
 * perf_event_open(2) syscall.
 *
 * SYSCALL:  __NR_perf_event_open == 298 on x86-64
 *           (defined in <asm/unistd_64.h>)
 *
 * WHAT IT DOES:
 *   The kernel exposes CPU performance counters (PMCs) through a
 *   file-descriptor based interface.  You configure the counter using
 *   struct perf_event_attr, call the syscall, and then read(2) the
 *   resulting fd to get a 64-bit counter value.
 *
 * WHY THIS MATTERS:
 *   Standard profilers (perf, VTune) all use this under the hood.
 *   Understanding it lets you build custom, zero-overhead
 *   instrumentation for latency-critical code.
 *
 * COMPILE:
 *   gcc -O2 -Wall -o 004_perf_event_open 004_perf_event_open.c
 *
 * USAGE:
 *   ./004_perf_event_open [pid] [duration_seconds]
 *   Default: PID=0 (self), duration=3
 *
 * OUTPUT:
 *   cycles, instructions, IPC, branches, branch-misses, branch-miss %
 * ====================================================================
 */

#define _GNU_SOURCE          /* See feature_test_macros(7) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>    /* syscall() wrapper */
#include <linux/perf_event.h> /* struct perf_event_attr, PERF_TYPE_* */
#include <stdint.h>
#include <errno.h>
#include <signal.h>

/*
 * --------------------------------------------------------------------
 * The perf_event_open(2) syscall is not wrapped in glibc (as of 2024),
 * so we provide our own thin wrapper.
 *
 * SYNOPSIS (from man 2 perf_event_open):
 *   long perf_event_open(
 *       struct perf_event_attr *attr,
 *       pid_t pid,
 *       int cpu,
 *       int group_fd,
 *       unsigned long flags
 *   );
 *
 * PARAMETERS:
 *   attr     – configuration structure (see below)
 *   pid      – 0 == current process / thread
 *              >0 == specific PID (needs permissions / cap_sys_ptrace)
 *              -1 == all processes (needs CAP_PERFMON / CAP_SYS_ADMIN)
 *   cpu      – -1 == any CPU, otherwise cpu >= 0 to pin to a core
 *   group_fd – -1 == standalone counter, otherwise fd of a group leader
 *   flags    – bitmask: PERF_FLAG_FD_CLOEXEC, PERF_FLAG_PID_CGROUP, etc.
 *
 * RETURN VALUE:
 *   A new file descriptor on success, -1 on error (errno is set).
 * --------------------------------------------------------------------
 */
static long perf_event_open_wrapper(struct perf_event_attr *attr,
                                    pid_t pid,
                                    int cpu,
                                    int group_fd,
                                    unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

/*
 * --------------------------------------------------------------------
 * Read a 64-bit counter value from a perf_event fd.
 *
 * The kernel's perf ring-buffer can be configured for sampling, but
 * for simple counting we just read(2) the fd.  The kernel returns a
 * 64-bit unsigned integer representing the number of events that
 * occurred since the fd was opened (or since the last ioctl reset).
 *
 * NOTE:  On 32-bit kernels the read returns a 32-bit value unless
 *        attr->read_format includes PERF_FORMAT_GROUP or _ID.
 * --------------------------------------------------------------------
 */
static uint64_t read_counter(int fd)
{
    uint64_t val = 0;
    ssize_t n = read(fd, &val, sizeof(val));
    if (n != sizeof(val)) {
        fprintf(stderr, "ERROR: read returned %zd (expected %zu)\n",
                n, sizeof(val));
        return 0;
    }
    return val;
}

/*
 * ====================================================================
 *                  H A R D W A R E   C O U N T E R S
 * ====================================================================
 *
 * Modern x86-64 CPUs implement a set of "Performance Monitoring
 * Counters" (PMCs).  These are special-purpose MSRs that the kernel
 * programs on our behalf via perf_event_open.
 *
 * The "type" field selects which family of counters to use:
 *
 *   PERF_TYPE_HARDWARE     (0) – Generic PMU events
 *   PERF_TYPE_SOFTWARE     (1) – Kernel-software events (context-switches, etc.)
 *   PERF_TYPE_TRACEPOINT   (2) – Ftrace tracepoints
 *   PERF_TYPE_HW_CACHE     (3) – L1/L2/L3 cache-access/miss counts
 *   PERF_TYPE_RAW          (4) – Raw CPU-specific event codes
 *
 * For PERF_TYPE_HARDWARE, the "config" field selects one of these
 * architectural events (defined in <linux/perf_event.h>):
 *
 *   PERF_COUNT_HW_CPU_CYCLES         (0)  –  Core clock cycles
 *                                            (halts on TSC stop)
 *   PERF_COUNT_HW_INSTRUCTIONS        (1)  –  Retired instructions
 *                                            (the "Instruction Count")
 *   PERF_COUNT_HW_CACHE_REFERENCES    (2)  –  Cache access (varies by uarch)
 *   PERF_COUNT_HW_CACHE_MISSES        (3)  –  Cache miss
 *   PERF_COUNT_HW_BRANCH_INSTRUCTIONS (4)  –  Retired branches
 *   PERF_COUNT_HW_BRANCH_MISSES       (5)  –  Mispredicted branches
 *   PERF_COUNT_HW_BUS_CYCLES          (6)  –  Bus (uncore) cycles
 *   ... (see kernel header for more)
 *
 * The kernel maps these to the appropriate CPU-specific MSR (e.g.
 * IA32_PERFCTR0 on P6, or the architectural PerfEvtSelN / PerfCntrN
 * MSRs on Core2/Nehalem/Sandybridge and later).  The exact MSR
 * numbers are microarchitecture-specific, but the kernel abstracts
 * this away.
 *
 * IMPORTANT:
 *   - On modern kernels and CPUs, these counters are "multiplexed"
 *     if you open more counters than physical PMCs exist (typically
 *     4-8).  The kernel time-stamps and scales the counts fairly.
 *     You can detect scaling by checking attr->read_format and using
 *     the PERF_FORMAT_TOTAL_TIME_ENABLED / _RUNNING flags.
 *   - Access may require:
 *       /proc/sys/kernel/perf_event_paranoid = 0 or 1
 *     (see snippet 014 for details).
 * ====================================================================
 */

/*
 * --------------------------------------------------------------------
 * Helper: open a hardware counter and return its file descriptor.
 *
 * We zero-initialize the struct with memset (important!), then set:
 *   .type        – PERF_TYPE_HARDWARE
 *   .size        – sizeof(struct perf_event_attr) for forward compat.
 *   .config      – one of the PERF_COUNT_HW_* constants above
 *   .disabled    – 1 means start disabled; we'll enable with ioctl
 *   .exclude_kernel – 1 means only count user-space events (safer
 *                  without CAP_PERFMON / CAP_SYS_ADMIN)
 *   .exclude_hv  – 1 means exclude events in the hypervisor
 *   .exclude_guest – 1 means exclude events in guest VMs
 *
 * We open for "pid" (0 = self) on "cpu" (-1 = any CPU).
 * --------------------------------------------------------------------
 */
static int open_hw_counter(unsigned int config, pid_t pid, int cpu)
{
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));          /* Zero everything first! */

    attr.type           = PERF_TYPE_HARDWARE; /* Use architectural PMCs */
    attr.size           = sizeof(attr);       /* Must be set for forward compat */
    attr.config         = config;             /* Which event to count */
    attr.disabled       = 1;                  /* Start disabled */
    attr.exclude_kernel = 1;                  /* User-space only (no root needed) */
    attr.exclude_hv     = 1;                  /* Don't count hypervisor */
    attr.exclude_guest  = 1;                  /* Don't count VMs */

    int fd = perf_event_open_wrapper(&attr, pid, cpu, -1, 0);
    if (fd == -1) {
        /*
         * Common errors:
         *   EACCES    – perf_event_paranoid > 1 and not root
         *   EINVAL    – invalid config / type combination
         *   EMFILE    – process fd limit reached
         *   ENOSPC    – no more hardware PMCs available on this CPU
         *   EPERM     – ptrace scope prevents monitoring other processes
         */
        fprintf(stderr, "perf_event_open(config=0x%x) failed: %m\n", config);
    }
    return fd;
}

/*
 * --------------------------------------------------------------------
 * Enable or disable a counter via ioctl(2).
 *
 *   PERF_EVENT_IOC_ENABLE  –  Start counting
 *   PERF_EVENT_IOC_DISABLE –  Stop counting
 *   PERF_EVENT_IOC_RESET   –  Zero the counter
 * --------------------------------------------------------------------
 */
static void counter_enable(int fd)
{
    ioctl(fd, PERF_EVENT_IOC_RESET,  0);   /* Reset to zero */
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);   /* Start counting */
}

static void counter_disable(int fd)
{
    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);  /* Stop counting */
}

/* ====================================================================
 *  M A I N
 *
 *  We open four counters: cycles, instructions, branches, branch-misses.
 *  Then we spin-loop for N seconds (doing a simple computation to
 *  generate some CPU activity), read the counters, and print results.
 * ==================================================================== */
int main(int argc, char *argv[])
{
    pid_t pid = 0;                          /* Default: monitor self */
    int duration = 3;                       /* Default: 3 seconds */

    if (argc > 1) pid = atoi(argv[1]);
    if (argc > 2) duration = atoi(argv[2]);

    printf("perf_event_open — Hardware Counter Demo\n");
    printf("PID: %d   Duration: %d seconds\n\n", pid, duration);
    printf("Counter Descriptions:\n");
    printf("  PERF_COUNT_HW_CPU_CYCLES         (0x%x) – Core clock cycles\n",
           PERF_COUNT_HW_CPU_CYCLES);
    printf("  PERF_COUNT_HW_INSTRUCTIONS       (0x%x) – Retired instructions\n",
           PERF_COUNT_HW_INSTRUCTIONS);
    printf("  PERF_COUNT_HW_BRANCH_INSTRUCTIONS(0x%x) – Retired branches\n",
           PERF_COUNT_HW_BRANCH_INSTRUCTIONS);
    printf("  PERF_COUNT_HW_BRANCH_MISSES      (0x%x) – Mispredicted branches\n\n",
           PERF_COUNT_HW_BRANCH_MISSES);

    /* ----- Open all four counters ----- */
    int fd_cyc = open_hw_counter(PERF_COUNT_HW_CPU_CYCLES,         pid, -1);
    int fd_ins = open_hw_counter(PERF_COUNT_HW_INSTRUCTIONS,       pid, -1);
    int fd_br  = open_hw_counter(PERF_COUNT_HW_BRANCH_INSTRUCTIONS, pid, -1);
    int fd_bm  = open_hw_counter(PERF_COUNT_HW_BRANCH_MISSES,      pid, -1);

    if (fd_cyc < 0 || fd_ins < 0 || fd_br < 0 || fd_bm < 0) {
        fprintf(stderr, "ERROR: could not open all counters.\n");
        fprintf(stderr, "Try: sudo sh -c 'echo 0 > /proc/sys/kernel/perf_event_paranoid'\n");
        exit(EXIT_FAILURE);
    }

    /* ----- Enable counting ----- */
    counter_enable(fd_cyc);
    counter_enable(fd_ins);
    counter_enable(fd_br);
    counter_enable(fd_bm);

    /*
     * ----- Workload: busy-loop -----
     * We'll do some integer multiplication to burn cycles and
     * generate a predictable number of instructions & branches.
     *
     * The volatile prevents the compiler from optimizing away the loop.
     */
    volatile unsigned long long sum = 0;
    for (int i = 0; i < duration * 100000000; i++) {
        sum += (unsigned long long)i * (unsigned long long)(i ^ 0xDEADBEEF);
        /* The if creates taken/not-taken branches */
        if (sum & 1) {
            sum ^= 0xFF;
        }
    }

    /* ----- Disable counters ----- */
    counter_disable(fd_cyc);
    counter_disable(fd_ins);
    counter_disable(fd_br);
    counter_disable(fd_bm);

    /* ----- Read all counters ----- */
    uint64_t cycles         = read_counter(fd_cyc);
    uint64_t instructions   = read_counter(fd_ins);
    uint64_t branches       = read_counter(fd_br);
    uint64_t branch_misses  = read_counter(fd_bm);

    /* ----- Compute derived metrics ----- */
    double ipc = (double)instructions / (double)cycles;

    /*
     * IPC = Instructions Per Cycle
     *   - Modern x86 cores can retire 4-6 instructions per cycle.
     *   - IPC < 1 means the pipeline stalls (cache misses, bad branch
     *     prediction, long-latency instructions).
     *   - IPC correlates directly with performance.
     */

    double bmr = (double)branch_misses / (double)branches * 100.0;

    /*
     * Branch Miss Rate:
     *   - Each misprediction flushes ~15-20 cycles of work on modern cores.
     *   - A rate > 2% is poor; >5% is catastrophic for performance.
     *   - Data-dependent branches (e.g., branch on random input) destroy
     *     prediction accuracy.
     */

    /* ----- Print results ----- */
    printf("==========  RESULTS  ==========\n");
    printf("Cycles:              %'20lu\n", cycles);
    printf("Instructions:        %'20lu\n", instructions);
    printf("IPC (instructions/cycle): %.4f\n", ipc);
    printf("Branches:            %'20lu\n", branches);
    printf("Branch-Misses:       %'20lu\n", branch_misses);
    printf("Branch Miss Rate:    %'18.2f%%\n", bmr);
    printf("===============================\n");
    printf("(sum = %llu, to prevent optimization)\n", sum);

    /* ----- Clean up ----- */
    close(fd_cyc);
    close(fd_ins);
    close(fd_br);
    close(fd_bm);

    return 0;
}
