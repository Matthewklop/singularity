/*
 * ====================================================================
 * CRITICAL SNIPPET: 011_immediate_shortening.c
 * ====================================================================
 * Shorten immediate operands: replace `add $imm32, %eax` with
 * `add $imm8, %eax` when the immediate fits in signed 8-bit range.
 *
 * THE PROBLEM:
 *   Some arithmetic instructions have two encodings:
 *
 *     LONG FORM:     opcode + imm32     (5 bytes total for %eax)
 *     SHORT FORM:    0x83 + ModRM + imm8 (3 bytes total for any reg)
 *
 *   For example:
 *     ADD $imm32, %eax    => 0x05 + 4 bytes imm32  (5 bytes)
 *     ADD $imm8,  %eax    => 0x83 0xC0 + 1 byte    (3 bytes)
 *                           (saves 2 bytes!)
 *
 *   Similarly:
 *     SUB $imm32, %eax    => 0x2D + 4 bytes imm32  (5 bytes)
 *     SUB $imm8,  %eax    => 0x83 0xE8 + 1 byte    (3 bytes)
 *
 * THE SHORT FORM (0x83 opcode):
 *   0x83 is the "ALU operation with sign-extended 8-bit immediate"
 *   opcode.  The ModRM byte encodes:
 *     - mod = 11 (register direct)
 *     - reg = operation (0=ADD, 5=SUB, etc.)
 *     - rm  = destination register
 *
 *   ModRM for ADD:
 *     reg=0 => ModRM = 0xC0 + rm  (0xC0=ADD EAX, 0xC1=ADD ECX, ...)
 *   ModRM for SUB:
 *     reg=5 => ModRM = 0xE8 + rm  (0xE8=SUB EAX, 0xE9=SUB ECX, ...)
 *
 *   The immediate is sign-extended from 8 to 32/64 bits, so imm8 must
 *   be in range [-128, 127].  Values 0..127 work directly; values
 *   128..255 would be sign-extended to negative numbers (wrong!).
 *
 * REX.W VARIANTS:
 *   When using 64-bit operands, the long form is:
 *     REX.W (0x48) + 0x05 + 4-byte imm32 sign-extended to imm64
 *     = 6 bytes
 *   The short form is:
 *     REX.W (0x48) + 0x83 + ModRM + imm8
 *     = 4 bytes
 *   Same imm8 check applies.
 *
 * WHY SHORTER IS BETTER:
 *   - Less instruction cache pressure (the L1I cache is only 32KB)
 *   - Each byte saved is one more byte of useful code that fits in cache
 *   - Saves decode bandwidth (fewer bytes to decode in parallel)
 *   - Slightly less instruction fetch bandwidth from the frontend
 *
 * COMPILE:
 *   gcc -O2 -Wall -o 011_immediate_shortening 011_immediate_shortening.c
 *
 * USAGE:
 *   ./011_immediate_shortening
 * ==================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>   /* for SCHAR_MIN, SCHAR_MAX */

/*
 * ====================================================================
 * OPCODE TABLES
 * ==================================================================== */

/*
 * Long forms for ADD/SUB with EAX accumulator (no ModRM needed):
 *   0x05 + imm32   => ADD EAX, imm32     (5 bytes)
 *   0x2D + imm32   => SUB EAX, imm32     (5 bytes)
 *
 * With REX.W:
 *   0x48 0x05 + imm32  => ADD RAX, imm32 (6 bytes, imm32 sign-extended)
 *   0x48 0x2D + imm32  => SUB RAX, imm32 (6 bytes)
 *
 * Short form (0x83 + ModRM + imm8):
 *   ModRM where reg field encodes the operation:
 *     0 = ADD, 1 = OR, 2 = ADC, 3 = SBB,
 *     4 = AND, 5 = SUB, 6 = XOR, 7 = CMP
 *
 *   ModRM = 0b11000reg + rm   (mod=11 for register direct)
 *
 *   For ADD:  reg=0 => 0b11000000 + rm = 0xC0 + rm
 *   For SUB:  reg=5 => 0b11010100 + rm = 0xE8 + rm
 *              0b11000reg = 0xC0 + (reg << 3)
 *              reg=5 => 0xC0 + 40 = 0xE8.  Check: 0b11000000 + (5 << 3) = 0xC0 + 0x28 = 0xE8. Yes.
 *
 *   Register-to-ModRM mapping:
 *     rm=0 (EAX):   ADD: 0xC0, SUB: 0xE8
 *     rm=1 (ECX):   ADD: 0xC1, SUB: 0xE9
 *     rm=2 (EDX):   ADD: 0xC2, SUB: 0xEA
 *     rm=3 (EBX):   ADD: 0xC3, SUB: 0xEB
 *     rm=4 (ESP):   ADD: 0xC4, SUB: 0xEC
 *     rm=5 (EBP):   ADD: 0xC5, SUB: 0xED
 *     rm=6 (ESI):   ADD: 0xC6, SUB: 0xEE
 *     rm=7 (EDI):   ADD: 0xC7, SUB: 0xEF
 */

/*
 * Check if a value fits in signed 8-bit range [-128, 127]
 * In practice, for unsigned 32-bit immediates, values up to 127
 * can use the short form.  Values 128..255 would be sign-extended
 * to negative numbers, which is WRONG for unsigned operations.
 *
 * For signed operations, any value in [-128, 127] is fine.
 */
static int fits_in_signed8(int32_t val)
{
    return (val >= -128 && val <= 127);
}

/*
 * --------------------------------------------------------------------
 * detect_and_suggest:
 *
 * Given a buffer, find patterns like:
 *   ADD $imm32, %eax:  05 xx xx xx xx  (5 bytes)
 *   SUB $imm32, %eax:  2D xx xx xx xx  (5 bytes)
 *   REX.W ADD RAX:     48 05 xx xx xx xx (6 bytes)
 *   REX.W SUB RAX:     48 2D xx xx xx xx (6 bytes)
 *
 * Check if imm32 fits in signed 8-bit.
 * If so, suggest the shorter encoding.
 * -------------------------------------------------------------------- */
static int detect_and_suggest(const unsigned char *buf, size_t len)
{
    int suggestions = 0;
    size_t i = 0;

    while (i < len) {
        int is_64bit = 0;
        size_t start = i;

        /* Check for REX.W prefix */
        if (i < len && buf[i] == 0x48) {
            is_64bit = 1;
            i++;
        }

        if (i >= len) break;

        unsigned char op = buf[i];
        const char *op_name = NULL;
        int found = 0;

        if (op == 0x05) {
            /* ADD EAX/RAX, imm32 */
            op_name = "ADD";
            if (i + 5 <= len) {  /* Opcode + 4-byte immediate */
                /* Read imm32 (little-endian) */
                int32_t imm = (int32_t)(buf[i+1] | (buf[i+2] << 8) |
                                        (buf[i+3] << 16) | (buf[i+4] << 24));
                int total_len = is_64bit ? 6 : 5;
                found = 1;

                printf("  Offset %4zu: ", start);
                if (is_64bit) printf("48 ");
                printf("05 ");
                for (int j = 1; j <= 4; j++) printf("%02X ", buf[i+j]);
                printf("  => %s %s, %d", op_name,
                       is_64bit ? "RAX" : "EAX", imm);

                if (fits_in_signed8(imm)) {
                    unsigned char modrm = 0xC0; /* ADD reg field=0, rm=0 (EAX) */
                    printf("\n             Can shorten to: ");
                    if (is_64bit) printf("48 ");
                    printf("83 %02X %02X", modrm, (unsigned char)(imm & 0xFF));
                    printf("  (%d bytes -> %d bytes, save %d bytes)\n",
                           total_len,
                           (is_64bit ? 4 : 3),
                           total_len - (is_64bit ? 4 : 3));
                    suggestions++;
                } else {
                    printf("  (imm32 doesn't fit in signed 8-bit)\n");
                }
            }
            i += (found ? 5 : 1);
            continue;
        }

        if (op == 0x2D) {
            /* SUB EAX/RAX, imm32 */
            op_name = "SUB";
            if (i + 5 <= len) {
                int32_t imm = (int32_t)(buf[i+1] | (buf[i+2] << 8) |
                                        (buf[i+3] << 16) | (buf[i+4] << 24));
                int total_len = is_64bit ? 6 : 5;
                found = 1;

                printf("  Offset %4zu: ", start);
                if (is_64bit) printf("48 ");
                printf("2D ");
                for (int j = 1; j <= 4; j++) printf("%02X ", buf[i+j]);
                printf("  => %s %s, %d", op_name,
                       is_64bit ? "RAX" : "EAX", imm);

                if (fits_in_signed8(imm)) {
                    unsigned char modrm = 0xE8; /* SUB reg field=5, rm=0 (EAX) */
                    printf("\n             Can shorten to: ");
                    if (is_64bit) printf("48 ");
                    printf("83 %02X %02X", modrm, (unsigned char)(imm & 0xFF));
                    printf("  (%d bytes -> %d bytes, save %d bytes)\n",
                           total_len,
                           (is_64bit ? 4 : 3),
                           total_len - (is_64bit ? 4 : 3));
                    suggestions++;
                } else {
                    printf("  (imm32 doesn't fit in signed 8-bit)\n");
                }
            }
            i += (found ? 5 : 1);
            continue;
        }

        /* Not a match, advance past the opcode we scanned */
        if (!found) {
            i = start + 1;
        }
    }

    return suggestions;
}

/*
 * ====================================================================
 * Also show the 0x83 + ModRM table for other reg fields beyond ADD/SUB
 * ==================================================================== */
static void print_op83_table(void)
{
    printf("\n=== Opcode 0x83: ALU with sign-extended imm8 ===\n");
    printf("Format: 0x83 ModRM imm8\n");
    printf("ModRM.reg field (bits 5-3) selects the operation:\n\n");

    const char *ops[8] = {
        "ADD", "OR", "ADC", "SBB",
        "AND", "SUB", "XOR", "CMP"
    };
    for (int reg = 0; reg < 8; reg++) {
        printf("  reg=%d (%d%d%d) = %s\n",
               reg, (reg>>2)&1, (reg>>1)&1, reg&1, ops[reg]);
    }

    printf("\nModRM bytes for each register (mod=11):\n");
    printf("  Reg  EAX(0)  ECX(1)  EDX(2)  EBX(3)  ESP(4)  EBP(5)  ESI(6)  EDI(7)\n");
    for (int reg = 0; reg < 8; reg++) {
        printf("  %s:  ", ops[reg]);
        for (int rm = 0; rm < 8; rm++) {
            unsigned char modrm = 0xC0 | (reg << 3) | rm;
            printf("  %02X   ", modrm);
        }
        printf("\n");
    }
}

/*
 * ====================================================================
 * MAIN
 *
 * We construct a test buffer with various ADD/SUB immediates (some
 * fitting in 8-bit, some not) and run the detector.
 * ==================================================================== */
int main(void)
{
    printf("============================================================\n");
    printf("  IMMEDIATE SHORTENING: ADD/SUB imm32 -> imm8\n");
    printf("============================================================\n\n");

    /* Build test buffer */
    unsigned char buf[256];
    size_t pos = 0;

    /* ADD EAX, 5      (0x05 + 05 00 00 00) — FITS in imm8 */
    buf[pos++] = 0x05;
    buf[pos++] = 0x05; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00;

    /* ADD EAX, 127    (0x05 + 7F 00 00 00) — FITS (boundary) */
    buf[pos++] = 0x05;
    buf[pos++] = 0x7F; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00;

    /* ADD EAX, 128    (0x05 + 80 00 00 00) — DOES NOT FIT (would sign-extend to -128) */
    buf[pos++] = 0x05;
    buf[pos++] = 0x80; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00;

    /* SUB EAX, 10     (0x2D + 0A 00 00 00) — FITS */
    buf[pos++] = 0x2D;
    buf[pos++] = 0x0A; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00;

    /* SUB EAX, -100   (0x2D + 9C FF FF FF) — FITS (negative -100 = 0xFFFFFF9C as signed) */
    buf[pos++] = 0x2D;
    buf[pos++] = 0x9C; buf[pos++] = 0xFF; buf[pos++] = 0xFF; buf[pos++] = 0xFF;

    /* SUB EAX, -200   (0x2D + 38 FF FF FF) — DOES NOT FIT (would be 0x38=56, not -200) */
    buf[pos++] = 0x2D;
    buf[pos++] = 0x38; buf[pos++] = 0xFF; buf[pos++] = 0xFF; buf[pos++] = 0xFF;

    /* REX.W ADD RAX, 42 (0x48 0x05 + 2A 00 00 00) — FITS */
    buf[pos++] = 0x48;
    buf[pos++] = 0x05;
    buf[pos++] = 0x2A; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00;

    /* REX.W SUB RAX, 99 (0x48 0x2D + 63 00 00 00) — FITS */
    buf[pos++] = 0x48;
    buf[pos++] = 0x2D;
    buf[pos++] = 0x63; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00;

    /* REX.W ADD RAX, 200 (0x48 0x05 + C8 00 00 00) — DOES NOT FIT */
    buf[pos++] = 0x48;
    buf[pos++] = 0x05;
    buf[pos++] = 0xC8; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00;

    size_t buf_len = pos;

    printf("Test buffer (%zu bytes):\n", buf_len);
    for (size_t i = 0; i < buf_len; i++)
        printf("%02X ", buf[i]);
    printf("\n\n");

    printf("=== Analysis ===\n\n");
    int suggestions = detect_and_suggest(buf, buf_len);

    printf("\n=== Summary ===\n");
    printf("  Found %d shortenings possible.\n\n", suggestions);

    print_op83_table();

    printf("\n============================================================\n");
    printf("  WHY SHORTEN IMMEDIATES?\n");
    printf("============================================================\n\n");

    printf("1. ICACHE PRESSURE\n");
    printf("   The L1 instruction cache is 32 KB on modern CPUs.\n");
    printf("   Saving 2 bytes per instruction means up to ~640 more\n");
    printf("   instructions fit in cache.  For hot inner loops, this\n");
    printf("   can be the difference between cache hit and miss.\n\n");

    printf("2. BYTES SAVED EXAMPLES:\n");
    printf("   Short form (3 bytes):  83 C0 xx\n");
    printf("   Long form  (5 bytes):  05 xx xx xx xx\n");
    printf("   Savings:               2 bytes (40%% reduction)\n\n");
    printf("   Short form REX.W (4):  48 83 C0 xx\n");
    printf("   Long form REX.W  (6):  48 05 xx xx xx xx\n");
    printf("   Savings:               2 bytes (33%% reduction)\n\n");

    printf("3. NO FUNCTIONAL DIFFERENCE\n");
    printf("   The sign-extended 8-bit immediate produces the EXACT\n");
    printf("   same result as the 32-bit immediate, as long as the\n");
    printf("   value fits in signed 8-bit.\n\n");

    printf("4. COMPILER BEHAVIOR\n");
    printf("   GCC and Clang with -Os (optimize for size) will use\n");
    printf("   0x83 encoding.  With -O2/-O3 they may keep the 5-byte\n");
    printf("   form if the immediate doesn't fit.  Some compilers\n");
    printf("   always prefer the shorter form when possible.\n\n");

    printf("5. SIGN EXTENSION RULES\n");
    printf("   imm8 (0x83 form) is sign-extended to 32/64 bits.\n");
    printf("   So values 0x00-0x7F become 0x00000000-0x0000007F,\n");
    printf("   but 0x80-0xFF become 0xFFFFFF80-0xFFFFFFFF.\n");
    printf("   This is correct for signed arithmetic with negative\n");
    printf("   values, but incorrect for unsigned arithmetic with\n");
    printf("   values >= 128 (they'd become large negative numbers\n");
    printf("   when sign-extended).\n\n");

    printf("6. APPLICABLE OPCODES\n");
    printf("   The optimization applies to: ADD, OR, ADC, SBB,\n");
    printf("   AND, SUB, XOR, CMP — all eight operations encoded\n");
    printf("   by the reg field of ModRM with opcode 0x83.\n\n");

    printf("7. PADDING WITH 2-BYTE NOP\n");
    printf("   If code size must be preserved (e.g., for jump offset\n");
    printf("    correctness), pad the saved space with a 2-byte NOP:\n");
    printf("   0x66 0x90.  This keeps total instruction count safe.\n");

    return 0;
}
