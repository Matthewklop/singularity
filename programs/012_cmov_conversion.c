/*
 * ====================================================================
 * CRITICAL SNIPPET: 012_cmov_conversion.c
 * ====================================================================
 * Convert conditional jumps to CMOVcc (conditional move) instructions
 * to eliminate branch misprediction penalties.
 *
 * THE PROBLEM:
 *   Branch misprediction flushes the pipeline, costing ~15-20 cycles
 *   on modern CPUs.  For unpredictable branches (e.g., data-dependent
 *   conditions on random input), this can devastate performance.
 *
 * THE SOLUTION: CMOVcc (Conditional MOVE)
 *   CMOVcc reads a condition code (like a conditional jump) but instead
 *   of changing control flow, it conditionally moves data between
 *   registers.  This eliminates the branch entirely!
 *
 *   CONDITION CODE MAPPING:
 *     CC  Mnemonic    Description
 *     0   O           Overflow
 *     1   B/NAE/C     Below / Not Above or Equal / Carry
 *     2   Z/E         Zero / Equal
 *     3   BE/NA       Below or Equal / Not Above
 *     4   S           Sign (negative)
 *     5   NS          Not Sign (non-negative)
 *     6   P/PE        Parity / Parity Even
 *     7   NP/PO       Not Parity / Parity Odd
 *     8   L/NGE       Less / Not Greater or Equal (signed)
 *     9   NL/GE       Not Less / Greater or Equal (signed)
 *     A   LE/NG       Less or Equal / Not Greater (signed)
 *     B   NLE/G       Not Less or Equal / Greater (signed)
 *
 *   CMOVcc encoding:
 *     0x0F 0x4[condition] ModRM  = 3 bytes total
 *     For example:
 *       CMOVE %eax, %ecx  => 0F 44 C1  (condition=2 for Z/E)
 *       CMOVNE %eax, %ecx => 0F 45 C1  (condition=5 for NZ/NE)
 *
 * COMPARE WITH CONDITIONAL JUMP:
 *   6-byte conditional jump near:  0F 8[condition] rel32  (6 bytes)
 *   CMOVcc:                        0F 4[condition] ModRM  (3 bytes)
 *   Savings: 3 bytes + zero branch misprediction!
 *
 * WHEN CAN WE CONVERT?
 *   A conditional jump can be replaced with CMOVcc when BOTH paths
 *   (taken and not-taken) assign the same result register.  The
 *   pattern is:
 *
 *     ; Both paths set %%rax:
 *     je  .Lfallthrough     ; jump if condition not met
 *     mov %%rbx, %%rax        ; path A
 *     jmp .Ldone
 *   .Lfallthrough:
 *     mov %%rcx, %%rax        ; path B
 *   .Ldone:
 *     ; continue with %%rax
 *
 *   If both paths write to %%rax, we can replace the whole thing with:
 *     cmovne %%rcx, %%rax     ; if condition false, move from %%rcx
 *                            ; (wait, that's not right...)
 *     cmove  %%rbx, %%rax     ; if condition true, move from %%rbx
 *
 *   Actually, the typical pattern is:
 *     ; Start with: %%rax has fallthrough value
 *     ; If condition is true, need to overwrite with taken-path value
 *     cmovcc %%rbx, %%rax    ; conditionally replace %%rax
 *
 *   The CMOV condition code is the SAME as the jump condition
 *   (not inverted).  So if the jump was JZ (jump-if-zero), use
 *   CMOVZ.
 *
 * COMPILE:
 *   gcc -O2 -Wall -o 012_cmov_conversion 012_cmov_conversion.c
 *
 * USAGE:
 *   ./012_cmov_conversion
 * ==================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>       /* for random test data */

/*
 * ====================================================================
 * CONDITION CODE TABLES
 * ==================================================================== */

static const char *cc_names[16] = {
    "O (overflow)",
    "B/NAE/C (below/not above-equal/carry)",
    "Z/E (zero/equal)",
    "BE/NA (below-equal/not above)",
    "S (sign/negative)",
    "NS (not sign/non-negative)",
    "P/PE (parity/parity-even)",
    "NP/PO (not-parity/parity-odd)",
    "L/NGE (less/not-greater-equal, signed)",
    "NL/GE (not-less/greater-equal, signed)",
    "LE/NG (less-equal/not-greater, signed)",
    "NLE/G (not-less-equal/greater, signed)",
};

/* Short one-letter codes for display */
static const char *cc_short[16] = {
    "O",  "B",  "Z",  "BE",
    "S",  "NS", "P",  "NP",
    "L",  "NL", "LE", "NLE",
    "??", "??", "??", "??"
};

/*
 * ====================================================================
 * CMOVcc ENCODING
 *
 * Format: 0x0F 0x4[condition] ModRM
 *
 * The condition is encoded in the low nibble of the second opcode byte:
 *   0x40 + CC (where CC is 0..15)
 *   e.g. CMOVE  => 0x0F 0x44
 *        CMOVNE => 0x0F 0x45
 *
 * The ModRM byte has:
 *   mod=11 (register direct)
 *   reg=source register
 *   rm=destination register
 *
 * For example: CMOVZ %ecx, %eax  (if ZF=1, set eax=ecx)
 *   0x0F 0x44 0xC1
 *   ModRM 0xC1 = 11 000 001
 *   mod=11, reg=0 (EAX is the SOURCE? WAIT!)
 *
 * Actually wait: CMOVcc format is:
 *   CMOVcc dest, src   (AT&T syntax: cmovcc %src, %dest)
 *   Opcode: 0F 4[cc] + ModRM
 *   ModRM: mod=11, reg=src, rm=dest
 *
 * So CMOVZ %ecx, %eax means: if ZF=1, EAX = ECX
 *   reg=1 (ECX = source), rm=0 (EAX = dest)
 *   ModRM = 0xC0 + (1<<3) + 0 = 0xC8
 *   Encoding: 0F 44 C8
 *
 * Let's verify: CMOVZ %eax, %eax (redundant but valid)
 *   reg=0 (EAX src), rm=0 (EAX dest) => ModRM = 0xC0
 *   0F 44 C0
 * ==================================================================== */

/*
 * Generate the 3-byte CMOVcc encoding for a given condition.
 * dest_reg and src_reg are 0-7 (EAX..EDI).
 * Returns the bytes in out buffer.
 */
static void gen_cmov(int condition, int dest_reg, int src_reg,
                     unsigned char *out)
{
    /* 0x0F escape + (0x40 | condition) */
    out[0] = 0x0F;
    out[1] = 0x40 | (condition & 0x0F);
    /* ModRM: mod=11 (0xC0) + reg<<3 + rm */
    out[2] = 0xC0 | ((src_reg & 7) << 3) | (dest_reg & 7);
}

/*
 * ====================================================================
 * Demonstrate CMOV vs Conditional Jump
 *
 * We'll simulate the classic if-then-else pattern:
 *
 *   int a = (condition) ? value_if_true : value_if_false;
 *
 * Branch version:
 *   cmp  %cond, %zero
 *   jne  .Lfalse
 *   mov  %true_val, %result   ; taken path
 *   jmp  .Ldone
 * .Lfalse:
 *   mov  %false_val, %result  ; fallthrough path
 * .Ldone:
 *   ; use %result
 *
 * CMOV version:
 *   mov  %false_val, %result  ; pre-load fallthrough
 *   cmp  %cond, %zero
 *   cmovne %true_val, %result ; conditionally overwrite
 *   ; use %result
 * ==================================================================== */

/*
 * Compare branch vs CMOV for a series of random conditions.
 * Count cycles using rdtscp (inline assembly).
 */
static inline unsigned long long rdtscp(void)
{
    unsigned int aux;
    unsigned long long lo, hi;
    __asm__ volatile ("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
    return lo | (hi << 32);
}

/*
 * Branch-based conditional select:
 *   result = (cond) ? a : b;
 * This will be compiled to a branch by most compilers at -O0.
 */
static int branch_select(int cond, int a, int b)
{
    if (cond)
        return a;
    else
        return b;
    /* Creates: cmp/test, jcc, mov, jmp, mov (or similar) */
}

/*
 * CMOV-based conditional select using inline assembly.
 */
static int cmov_select(int cond, int a, int b)
{
    int result = b;  /* Start with fallthrough value */
    __asm__ volatile (
        "test %[c], %[c]\n\t"
        "cmovnz %[a], %[r]\n\t"
        : [r] "+r" (result)
        : [a] "r" (a),
          [c] "r" (cond)
        : "cc"
    );
    return result;
}

/*
 * ====================================================================
 * MAIN
 * ==================================================================== */
int main(void)
{
    printf("============================================================\n");
    printf("  CMOV CONVERSION DEMO\n");
    printf("============================================================\n\n");

    printf("Conditional Move (CMOVcc) Encoding Reference:\n");
    printf("  Format: 0x0F 0x4C ModRM\n");
    printf("  where C = condition code (0xF for the 4-bit condition)\n\n");

    printf("Condition Code Table:\n");
    printf("  CC  Mnemonic   CMOV Encoding    Description\n");
    printf("  --  --------   ---------------  ------------------------------\n");
    for (int cc = 0; cc < 16; cc++) {
        unsigned char bytes[3];
        gen_cmov(cc, 0, 0, bytes);
        printf("  %02X   %-8s   0F %02X %02X        %s\n",
               cc, cc_short[cc], bytes[1], bytes[2], cc_names[cc]);
    }

    /* Show CMOV for different registers */
    printf("\nCMOVcc with different register pairs (cc=2, Z/E):\n");
    printf("  CMOVZ EAX, ECX  => 0F 44 C8  (src=ECX(1), dst=EAX(0))\n");
    printf("  CMOVZ EAX, EDX  => 0F 44 D0  (src=EDX(2), dst=EAX(0))\n");
    printf("  CMOVZ EBX, EAX  => 0F 44 D8  (src=EAX(0), dst=EBX(3))\n");
    printf("  CMOVZ ECX, EAX  => 0F 44 C1  (src=EAX(0), dst=ECX(1))\n\n");

    /*
     * Show the jump-to-CMOV conversion pattern
     */
    printf("============================================================\n");
    printf("  CONVERSION EXAMPLE\n");
    printf("============================================================\n\n");

    printf("ORIGINAL CODE (with branch):\n");
    printf("  cmp  $0, %%rdi           ; compare condition\n");
    printf("  je   .Lfalse             ; 6-byte near jump: 0F 84 rel32\n");
    printf("  mov  %%rsi, %%rax         ; path A (condition true)\n");
    printf("  jmp  .Ldone\n");
    printf(".Lfalse:\n");
    printf("  mov  %%rdx, %%rax         ; path B (condition false)\n");
    printf(".Ldone:\n");
    printf("  ; rax has result\n\n");

    printf("OPTIMIZED CODE (with CMOV):\n");
    printf("  mov  %%rdx, %%rax         ; pre-load fallthrough value\n");
    printf("  cmp  $0, %%rdi           ; same condition\n");
    printf("  cmovne %%rsi, %%rax       ; 3-byte CMOVNE: 0F 45 C6\n");
    printf("  ; rax has result, NO BRANCH!\n\n");

    printf("BYTES SAVED:\n");
    printf("  Branch version: ~18 bytes (jne+mov+jmp+mov)\n");
    printf("  CMOV version:   ~12 bytes (mov+cmp+cmov)\n");
    printf("  Savings:        ~6 bytes\n\n");

    /*
     * PERFORMANCE COMPARISON
     */
    printf("============================================================\n");
    printf("  PERFORMANCE COMPARISON\n");
    printf("============================================================\n\n");

    /* Generate random conditions (predictable vs unpredictable) */
    srand(42);
    #define N 100000

    int *conditions = malloc(N * sizeof(int));
    int *a_vals = malloc(N * sizeof(int));
    int *b_vals = malloc(N * sizeof(int));
    int *results_branch = malloc(N * sizeof(int));
    int *results_cmov  = malloc(N * sizeof(int));

    /* Random inputs */
    for (int i = 0; i < N; i++) {
        conditions[i] = rand() & 1;       /* 50% true, 50% false (UNPREDICTABLE) */
        a_vals[i] = rand() & 0xFF;
        b_vals[i] = rand() & 0xFF;
    }

    /* Test BRANCH version */
    unsigned long long t1 = rdtscp();
    for (int i = 0; i < N; i++) {
        results_branch[i] = branch_select(conditions[i], a_vals[i], b_vals[i]);
    }
    unsigned long long t2 = rdtscp();
    double branch_cycles = (double)(t2 - t1) / N;

    /* Test CMOV version */
    unsigned long long t3 = rdtscp();
    for (int i = 0; i < N; i++) {
        results_cmov[i] = cmov_select(conditions[i], a_vals[i], b_vals[i]);
    }
    unsigned long long t4 = rdtscp();
    double cmov_cycles = (double)(t4 - t3) / N;

    /* Verify correctness */
    int errors = 0;
    for (int i = 0; i < N; i++) {
        if (results_branch[i] != results_cmov[i]) {
            errors++;
        }
    }

    printf("Test size: %d iterations\n\n", N);
    printf("Branch (conditional jump) version:\n");
    printf("  Total cycles: %llu\n", t2 - t1);
    printf("  Cycles/iteration: %.2f\n\n", branch_cycles);

    printf("CMOV (conditional move) version:\n");
    printf("  Total cycles: %llu\n", t4 - t3);
    printf("  Cycles/iteration: %.2f\n\n", cmov_cycles);

    printf("Performance difference: %.1f%%\n",
           ((branch_cycles - cmov_cycles) / branch_cycles) * 100.0);

    if (errors == 0)
        printf("✓ Results match: CMOV is functionally equivalent!\n\n");
    else
        printf("✗ MISMATCHES: %d (CMOV bug!)\n\n", errors);

    printf("ERRORS: %d (should be 0)\n\n", errors);

    /*
     * EXPLANATION
     */
    printf("============================================================\n");
    printf("  WHY CMOV BEATS BRANCHES FOR UNPREDICTABLE DATA\n");
    printf("============================================================\n\n");

    printf("BRANCH MISPREDICTION PENALTY:\n");
    printf("  Modern CPUs have deep pipelines (14-19 stages).\n");
    printf("  When the branch predictor guesses wrong:\n");
    printf("    1. All instructions after the branch are flushed.\n");
    printf("    2. The frontend must be restarted at the correct target.\n");
    printf("    3. Pipeline refill takes ~15-20 cycles.\n");
    printf("  For a 50%% unpredictable branch, pipeline utilization drops\n");
    printf("  to roughly 50%% (wastes ~10 cycles per branch on avg).\n\n");

    printf("CMOV HAS NO BRANCH:\n");
    printf("  CMOVcc is a simple ALU instruction with a condition input.\n");
    printf("  It executes in 1 cycle on most CPUs.\n");
    printf("  It consumes execution resources but NOT frontend bandwidth\n");
    printf("  for misprediction recovery.\n\n");

    printf("WHEN TO USE CMOV:\n");
    printf("  ✓ Data-dependent branches (condition = random input)\n");
    printf("  ✓ Both paths are short (1-3 instructions)\n");
    printf("  ✓ Both paths write the same register\n");
    printf("  ✓ Latency of CMOV doesn't create a critical path\n\n");

    printf("WHEN NOT TO USE CMOV:\n");
    printf("  ✗ Highly predictable branches (99%%+ prediction rate)\n");
    printf("  ✗ Long taken path with memory accesses (CMOV always\n");
    printf("    reads both operands, which may cause cache misses)\n");
    printf("  ✗ Floating-point code (requires SSE/AVX blend instructions)\n\n");

    printf("TRADE-OFF:\n");
    printf("  Branch:  mispredict = 15-20 cycles, correct = ~0 extra\n");
    printf("  CMOV:    always = 1 cycle\n");
    printf("  Break-even: ~5%% misprediction rate (roughly)\n");

    free(conditions);
    free(a_vals);
    free(b_vals);
    free(results_branch);
    free(results_cmov);

    return 0;
}
