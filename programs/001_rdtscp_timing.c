// ============================================================
// CRITICAL SNIPPET
// NAME: rdtscp_timing
// CATEGORY: 000-099 Timing & Performance
// WHAT: Measure CPU cycles with the RDTSCP instruction
// WHY:  RDTSCP reads the CPU's internal cycle counter (Time Stamp
//       Counter). It's a 64-bit counter that increments every CPU
//       cycle. RDTSCP is serializing (waits for previous instructions
//       to complete) unlike plain RDTSC. This gives nanosecond-
//       precision timing without OS calls.
//
//       The instruction returns:
//         EDX:EAX = 64-bit cycle count
//         ECX     = CPU core ID (for detecting migration)
//
// BUILD: gcc -O3 -o 001_rdtscp_timing 001_rdtscp_timing.c -lm
// RUN: ./001_rdtscp_timing
// DEPENDS: none
// ============================================================
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

// Read the CPU's cycle counter with serialization.
// The "rdtscp" instruction atomically reads the TSC and stores
// the 64-bit value across EDX (high 32) and EAX (low 32).
// The "cpuid" variant (rdtscp) also stores the core ID in ECX.
//
// We use inline assembly with:
//   "=a"(lo)  — output: EAX → lo (lower 32 bits)
//   "=d"(hi)  — output: EDX → hi (upper 32 bits)
//   : : "ecx" — clobbers: ECX (core ID, we discard it)
//
// The "volatile" prevents the compiler from reordering or
// optimizing away the instruction.
static inline uint64_t rdtscp(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) : : "ecx");
    // Combine high and low into a single 64-bit value.
    // hi contains the upper 32 bits, lo the lower 32.
    // Shift hi left by 32 and OR with lo.
    return ((uint64_t)hi << 32) | lo;
}

// Read the TSC WITHOUT serialization — faster but less accurate.
// RDTSC does NOT wait for previous instructions to finish,
// so it may undercount cycles if the pipeline is still flushing.
// Useful for relative measurements (before/after) where the
// pipeline state is the same for both reads.
//
// Same register layout: EDX:EAX = 64-bit counter.
// No ECX clobber because plain RDTSC doesn't return core ID.
//
// Intel SDM: "The processor monotonically increments the
// time-stamp counter MSR every clock cycle, and resets it to 0
// whenever the processor is reset."
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

int main(void) {
    printf("RDTSCP Timing Demonstration\n");
    printf("==========================\n\n");

    // ---- 1. Measure CPU frequency ----
    // RDTSCP before and after a 1-second sleep gives us the
    // number of cycles in one second = CPU frequency in Hz.
    //
    // Sleep(1) is used because it's a reliable 1-second wallclock
    // delay. The cycle count difference equals the CPU speed.
    //
    // Caveat: The CPU may have changed frequency during sleep
    // (frequency scaling), so this is an estimate.
    // Modern CPUs with invariant TSC (constant frequency TSC)
    // are not affected by frequency changes.
    uint64_t t0 = rdtscp();
    sleep(1);
    uint64_t t1 = rdtscp();
    printf("Cycles in 1 second sleep: %lu\n", t1 - t0);
    // Divide by 1e9 to convert to GHz: cycles / (cycles/sec) = sec
    printf("Estimated CPU frequency:  %.2f GHz\n\n", (double)(t1 - t0) / 1e9);

    // ---- 2. Instruction overhead ----
    // The minimum measurable time is the cost of rdtsc/rdtscp itself.
    // Back-to-back rdtsc (no serialization) shows ~24 cycles on modern
    // CPUs (the time for rdtsc to execute twice).
    // Back-to-back rdtscp shows ~50 cycles because it includes the
    // serialization overhead of waiting for the pipeline to drain.
    printf("RDTSC (no serialization):\n");
    uint64_t a = rdtsc();
    uint64_t b = rdtsc();
    printf("  Back-to-back rdtsc delta: %lu cycles\n", b - a);

    printf("\nRDTSCP (with serialization):\n");
    a = rdtscp();
    b = rdtscp();
    printf("  Back-to-back rdtscp delta: %lu cycles\n", b - a);

    // ---- 3. Loop overhead measurement ----
    // Measuring a simple loop demonstrates how to use rdtscp for
    // benchmarking code sections. The volatile keyword prevents the
    // compiler from optimizing away the loop (which would make it 0 cycles).
    //
    // The empty loop does nothing useful, but its cycle count shows
    // the overhead of loop control (increment, compare, branch) per iteration.
    printf("\nEmpty loop overhead:\n");
    t0 = rdtscp();
    for (volatile int i = 0; i < 1000; i++) {}
    t1 = rdtscp();
    printf("  1000 iterations: %lu cycles (%.2f per iteration)\n",
           t1 - t0, (double)(t1 - t0) / 1000.0);

    // ---- 4. Why this matters ----
    // RDTSCP is the foundation of all microbenchmarks and performance
    // measurement in our live improver. Every optimization we apply
    // (NOP consolidation, CMOV conversion, prefetch) is measured by
    // comparing rdtscp deltas before and after. Without this, we're
    // guessing. With it, we have hard cycle counts.
    printf("\nUse this to benchmark any code section:\n");
    printf("  uint64_t start = rdtscp();\n");
    printf("  // code to measure\n");
    printf("  uint64_t end = rdtscp();\n");
    printf("  printf(\"%%lu cycles\\n\", end - start);\n");

    return 0;
}
