/*
 * ====================================================================
 * CRITICAL SNIPPET: 005_nop_consolidation.c
 * ====================================================================
 * Multi-byte NOP consolidation — replaces runs of 0x90 (single-byte
 * NOP) with optimal multi-byte NOP equivalent sequences.
 *
 * WHY MULTI-BYTE NOPS ARE BETTER:
 *   The x86-64 architecture has variable-length instructions.  The
 *   single-byte NOP (opcode 0x90) is simply "XCHG EAX, EAX" which
 *   the CPU decodes as one uop that writes to EAX (creating a false
 *   dependency on older µarchs).  Multi-byte NOPs (0x0F 0x1F /0)
 *   are special-cased in the frontend:
 *
 *   1. FEWER DECODE UOPS:
 *      The CPU's instruction decoder has a limited bandwidth (4-6
 *      instructions per cycle).  One 5-byte NOP decodes as a single
 *      µop; five 1-byte NOPs consume 5x decoder slots.
 *
 *   2. BETTER FRONTEND THROUGHPUT:
 *      The DSB (Decoded Stream Buffer, aka µop cache) can hold many
 *      more µops per cache line if multi-byte NOPs collapse to one
 *      µop each, rather than many.
 *
 *   3. LESS ICACHE PRESSURE:
 *      Same number of bytes but fewer micro-ops to track.  The
 *      instruction cache (L1I) is typically 32 KB; 5 bytes is 5
 *      bytes regardless of encoding, but the decode pipeline is
 *      the bottleneck, not just icache.
 *
 *   4. AVOIDS FALSE DEPENDENCIES:
 *      NOP (0x90) = XCHG EAX,EAX, which on older CPUs creates a
 *      write to EAX.  The 0x0F 0x1F /0 NOPs are true no-ops with
 *      no destination register.
 *
 * COMPILE:
 *   gcc -O2 -Wall -o 005_nop_consolidation 005_nop_consolidation.c
 *
 * USAGE:
 *   ./005_nop_consolidation
 * ====================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*
 * ====================================================================
 * MULTI-BYTE NOP TABLE  (Intel Architecture SDM, Vol. 2A, Table A-6)
 * ====================================================================
 *
 * These encodings use opcode 0x0F 0x1F with a ModRM byte that
 * encodes "/0" (mod=00, reg=0, rm=...).  The patterns are chosen so
 * that the CPU treats them as true no-ops that decode to zero µops
 * that rename away (on modern µarchs) or a single µop with no
 * destination.
 *
 * Length  Encoding (hex)                         Description
 * ------  -------------------------------------  --------------------
 * 1       0x90                                   XCHG EAX,EAX (legacy)
 * 2       0x66 0x90                              2-byte NOP
 * 3       0x0F 0x1F 0x00                        3-byte NOP
 * 4       0x0F 0x1F 0x40 0x00                    4-byte NOP (ModRM SIB-like)
 * 5       0x0F 0x1F 0x44 0x00 0x00               5-byte NOP (disp8)
 * 6       0x66 0x0F 0x1F 0x44 0x00 0x00          6-byte NOP (0x66 prefix)
 * 7       0x0F 0x1F 0x80 0x00 0x00 0x00 0x00     7-byte NOP (disp32)
 * 8       0x0F 0x1F 0x84 0x00 0x00 0x00 0x00 0x00  8-byte NOP (SIB+disp32)
 * 9+      Composed from multiple NOPs if needed    >8 = not optimal
 *
 * NOTE: 2-byte NOP (0x66 0x90) uses the operand-size override prefix
 *       (0x66) to create a different instruction: "XCHG AX,AX" (16-bit).
 *       This is still a true NOP, just encoded differently.
 *
 * The official Intel recommended sequence for 9+ bytes is to use a
 * multi-byte NOP for the first 8 bytes and a shorter NOP for the
 * remainder.  For example, a 10-byte NOP = 8-byte NOP + 2-byte NOP.
 * ====================================================================
 */

/* Table indexed by NOP length (1 to 8).  Each entry is a byte count
 * followed by the actual instruction bytes.  We store them as arrays. */
static const unsigned char nop_table[9][9] = {
    {0},                                    /* index 0 unused */
    {0x90},                                 /* 1 byte  */
    {0x66, 0x90},                           /* 2 bytes */
    {0x0F, 0x1F, 0x00},                     /* 3 bytes */
    {0x0F, 0x1F, 0x40, 0x00},               /* 4 bytes */
    {0x0F, 0x1F, 0x44, 0x00, 0x00},         /* 5 bytes */
    {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},   /* 6 bytes */
    {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},        /* 7 bytes */
    {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},  /* 8 bytes */
};

/* Number of entries in the table */
#define NOP_TABLE_SIZE (sizeof(nop_table) / sizeof(nop_table[0]))  /* == 9 */

/*
 * --------------------------------------------------------------------
 * Print bytes in hex, with optional offset cursor.
 * --------------------------------------------------------------------
 */
static void print_hex(const unsigned char *buf, size_t len,
                      const char *label)
{
    printf("%s [%zu bytes]: ", label, len);
    for (size_t i = 0; i < len; i++)
        printf("%02X ", buf[i]);
    printf("\n");
}

/*
 * --------------------------------------------------------------------
 * Scan a buffer for runs of 0x90 (single-byte NOPs).
 *
 * For each run of length >= 2, replace it with the optimal multi-byte
 * NOP sequence of the same total length.
 *
 * The replacement is done by decomposing the run length into a sum of
 * entries from the NOP table (using the longest entries first).  This
 * minimizes the number of instructions emitted.
 *
 * EXAMPLE:
 *   Run of 5 NOPs:  90 90 90 90 90
 *   Replace with:   0F 1F 44 00 00   (5-byte NOP, 1 instruction)
 *
 *   Run of 7 NOPs:  90 90 90 90 90 90 90
 *   Replace with:   0F 1F 80 00 00 00 00  (7-byte NOP, 1 instruction)
 *
 *   Run of 9 NOPs:  (not in table), so:
 *   Replace with:   [8-byte NOP] + [1-byte NOP]
 *   = 0F 1F 84 00 00 00 00 00 90
 * --------------------------------------------------------------------
 */
static size_t consolidate_nops(unsigned char *buf, size_t len)
{
    size_t i = 0;            /* Current position in the buffer */
    size_t replaced = 0;     /* Count of single-byte NOPs replaced */

    while (i < len) {
        if (buf[i] == 0x90) {
            /* Found the start of a NOP run */
            size_t run_start = i;

            /* Count how many consecutive 0x90 bytes */
            while (i < len && buf[i] == 0x90)
                i++;

            size_t run_len = i - run_start;

            if (run_len >= 2) {
                /* Replace the run with optimal NOP sequence */

                /* We'll write the replacement into a temporary buffer
                 * to avoid corrupting the source as we read it. */
                unsigned char replacement[256]; /* More than enough */
                size_t rlen = 0;
                size_t remaining = run_len;

                /* Decompose 'remaining' into NOP table entries.
                 * Use the largest possible NOP (8 bytes) first. */
                while (remaining > 0) {
                    /* Pick the largest table entry <= remaining */
                    int pick = (remaining < NOP_TABLE_SIZE)
                                   ? (int)remaining
                                   : (int)(NOP_TABLE_SIZE - 1);
                    /* But cap at max entry in table (8) */
                    if (pick > 8) pick = 8;

                    /* Copy the NOP bytes */
                    memcpy(replacement + rlen, nop_table[pick], (size_t)pick);
                    rlen += (size_t)pick;
                    remaining -= (size_t)pick;
                }

                /* Replace the original run bytes with our optimized NOPs.
                 * run_len == rlen, so the total buffer length is preserved. */
                memcpy(buf + run_start, replacement, rlen);

                printf("  Run at offset %zu length %zu: ", run_start, run_len);
                print_hex(replacement, rlen, "replaced with");
                replaced++;
            }
        } else {
            i++;  /* Not a NOP, skip */
        }
    }
    return replaced;
}

/*
 * ====================================================================
 *  D E M O   B U F F E R
 *
 * We construct a small buffer containing various NOP runs interleaved
 * with other instructions (simulated here as marker bytes 0xCC = INT3).
 * ==================================================================== */
#define BUF_SIZE 128

int main(void)
{
    unsigned char buffer[BUF_SIZE];
    size_t pos = 0;

    printf("============================================================\n");
    printf("  NOP CONSOLIDATION DEMO\n");
    printf("============================================================\n\n");

    printf("Building demo buffer with NOP runs...\n\n");

    /* Fill with 0xCC (INT3) initially as "don't care" filler */
    memset(buffer, 0xCC, BUF_SIZE);

    pos = 0;

    /* Insert a 1-byte NOP (should NOT be consolidated) */
    buffer[pos++] = 0x90;

    /* Insert a 3-byte NOP run (should consolidate to 3-byte NOP) */
    buffer[pos++] = 0x90;
    buffer[pos++] = 0x90;
    buffer[pos++] = 0x90;

    /* Insert a 5-byte NOP run (should consolidate to 5-byte NOP) */
    for (int j = 0; j < 5; j++) buffer[pos++] = 0x90;

    /* Insert a 10-byte NOP run (should consolidate to 8+2) */
    for (int j = 0; j < 10; j++) buffer[pos++] = 0x90;

    /* Two separate runs with some non-NOP bytes between them */
    for (int j = 0; j < 4; j++) buffer[pos++] = 0x90;
    buffer[pos++] = 0xCC;  /* non-NOP */
    for (int j = 0; j < 4; j++) buffer[pos++] = 0x90;

    size_t original_len = pos;

    printf("=== ORIGINAL BUFFER ===\n");
    print_hex(buffer, original_len, "Buffer");
    printf("\n");

    printf("=== CONSOLIDATING NOP RUNS... ===\n");
    size_t replaced = consolidate_nops(buffer, original_len);

    printf("\n=== OPTIMIZED BUFFER ===\n");
    print_hex(buffer, original_len, "Buffer");
    printf("\n");

    printf("Replaced %zu NOP runs.\n", replaced);
    printf("\n");

    /*
     * ================================================================
     * EXPLANATION
     * ================================================================
     */
    printf("============================================================\n");
    printf("  WHY MULTI-BYTE NOPS ARE BETTER\n");
    printf("============================================================\n\n");

    printf("1. DECODE EFFICIENCY\n");
    printf("   The x86 frontend decodes instructions in parallel.\n");
    printf("   CPU decoders typically handle 4-6 instructions/cycle.\n");
    printf("   Five 1-byte NOPs:  5 decode slots consumed.\n");
    printf("   One  5-byte NOP:   1 decode slot consumed.\n");
    printf("   Result: ~5x better frontend throughput for NOP sequences.\n\n");

    printf("2. MICRO-OP CACHE (DSB / LSD)\n");
    printf("   The Decoded Stream Buffer caches decoded micro-ops.\n");
    printf("   Each 1-byte NOP produces 1 µop (with a false-dependency\n");
    printf("   on EAX in some implementations).\n");
    printf("   Multi-byte NOPs produce 0 real µops (they're recognized\n");
    printf("   and discarded at decode).\n\n");

    printf("3. ICACHE PRESSURE\n");
    printf("   Same byte count either way (5 bytes = 5 bytes), BUT\n");
    printf("   the decode bottleneck is often the bigger problem.\n");
    printf("   Still, multi-byte NOPs tend to align instructions\n");
    printf("   better for the L1I cache line boundaries.\n\n");

    printf("4. NO FALSE DEPENDENCIES\n");
    printf("   0x90 = XCHG EAX,EAX.  On P6-family and some newer\n");
    printf("   implementations, this creates a write to EAX that\n");
    printf("   other instructions may have to wait for.\n");
    printf("   0x0F 0x1F /0 NOPs have no destination register.\n\n");

    printf("5. PRACTICAL USE\n");
    printf("   Compilers (GCC, Clang, MSVC) emit multi-byte NOPs for\n");
    printf("   alignment padding.  Linkers (GNU ld '--optimize-nops')\n");
    printf("   consolidate existing NOPs.  Kernel hotpatch code uses\n");
    printf("   multi-byte NOPs for live patching safety.\n\n");

    printf("   The GCC flag '-mmanual-endbr' and 'align-labels' both\n");
    printf("   rely on multi-byte NOPs for efficient padding.\n");

    return 0;
}
