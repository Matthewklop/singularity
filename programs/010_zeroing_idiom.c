/*
 * ====================================================================
 * CRITICAL SNIPPET: 010_zeroing_idiom.c
 * ====================================================================
 * Find and fix inefficient zeroing: replace `mov $0, %reg` with
 * `xor %reg, %reg`.
 *
 * PROBLEM:
 *   Inefficient code often uses:
 *     mov $0, %%eax       (opcode: 0xB8 + reg, followed by 4 zero bytes)
 *     mov $0, %%rcx       (REX.W + 0xB9 + 8 zero bytes = 10 bytes)
 *
 *   These are 5-10 byte encodings that perform a direct load of zero
 *   into a register.  The constant zero must be fetched from the
 *   instruction stream (or a literal pool), which consumes a load
 *   port and creates a uop that must execute.
 *
 * SOLUTION:
 *   xor %reg, %reg       (opcode: 0x31 0xC0+reg*8+reg = 2 bytes)
 *
 *   For 64-bit: REX.W + 0x31 0xC0+reg*8+reg = 3 bytes.
 *
 *   The XOR idiom is:
 *     0x31 0xC0  →  XOR EAX,EAX   (2 bytes)
 *     0x31 0xC9  →  XOR ECX,ECX   (2 bytes)
 *     0x31 0xD2  →  XOR EDX,EDX   (2 bytes)
 *     0x31 0xDB  →  XOR EBX,EBX   (2 bytes)
 *     0x31 0xE4  →  XOR ESP,ESP   (2 bytes) -- DANGER: zeroes stack ptr!
 *     0x31 0xED  →  XOR EBP,EBP   (2 bytes)
 *     0x31 0xF6  →  XOR ESI,ESI   (2 bytes)
 *     0x31 0xFF  →  XOR EDI,EDI   (2 bytes)
 *
 * WHY XOR IS BETTER:
 *   Modern CPUs (Intel Core 2+, AMD K8+) implement a "register
 *   renaming optimization" specifically for XOR R,R and SUB R,R:
 *
 *   - The CPU detects that the instruction is a zeroing idiom
 *     (destination == source for XOR).
 *   - Instead of executing the XOR in the ALU (which would consume
 *     execution resources), the CPU simply renames the architectural
 *     register to a new zero-initialized physical register.
 *   - This is ZERO µOP COST — the instruction disappears from the
 *     execution pipeline entirely!
 *   - No execution port is used, no bypass delay, no dependency.
 *
 *   Compare with MOV $0, %reg which:
 *     - Requires the immediate to be fetched from the instruction
 *       cache and decoded.
 *     - Goes through the execution unit (ALU or load port).
 *     - Typically takes 1 µop that competes for execution ports.
 *     - Still has a dependency on the previous value of the register
 *       (since MOV overwrites it, but the old value must be known).
 *
 *   On older architectures, XOR clearing also breaks false dependencies
 *   on the previous register value.  For example, if you do:
 *     add %%eax, %%ebx    ; ebx depends on eax
 *     xor %%eax, %%eax    ; breaks dependency chain
 *   The XOR removes any false dependency that later instructions may
 *   have on the old EAX value.
 *
 * ENCODING DETAIL:
 *   The XOR r/m32, r32 opcode is 0x31, with a ModRM byte encoding:
 *     ModRM = [mod=11][reg=src][rm=dst]
 *     For XOR EAX,EAX: mod=11, reg=0 (EAX as src), rm=0 (EAX as dst)
 *     => ModRM = 11 000 000 = 0xC0 = 0b11000000
 *
 *   General formula: XOR R,R => 0x31 0xC0 + (reg*8 + reg)
 *   where reg = 0..7 for EAX..EDI.
 *
 *   For 64-bit zeroing, prefix with REX.W (0x48):
 *     XOR RAX,RAX => 48 31 C0 (3 bytes)
 *
 * HANDLING THE SIZE DIFFERENCE:
 *   Since XOR R,R is only 2 bytes (or 3 with REX), we need to pad
 *   with a 3-byte NOP (0x0F 0x1F 0x00) to keep the same total length
 *   if needed for jump offset preservation.  But of course, shorter
 *   is better for icache pressure!  The padding is only needed in
 *   patching scenarios where you can't change total code size.
 *
 * BYTES SAVED:
 *   MOV $0, %%eax (5 bytes) -> XOR EAX,EAX (2 bytes)  => saves 3 bytes
 *   MOV $0, %rax (10 bytes)-> XOR RAX,RAX (3 bytes)   => saves 7 bytes!
 *   MOV $0, %%rcx (10 bytes)-> XOR RCX,RCX (3 bytes)   => saves 7 bytes!
 *
 * COMPILE:
 *   gcc -O2 -Wall -o 010_zeroing_idiom 010_zeroing_idiom.c
 *
 * USAGE:
 *   ./010_zeroing_idiom
 * ==================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/*
 * ====================================================================
 * OPCODE TABLES FOR ZEROING
 * ==================================================================== */

/* The MOV $0, %r32 encoding: 0xB8 + reg (0..7) followed by 4 zero bytes.
 * 0xB8 = MOV EAX, imm32; 0xB9 = MOV ECX, imm32; ... 0xBF = MOV EDI, imm32
 * Then 4 bytes of the immediate (00 00 00 00 for zero). */
static int is_mov_zero_32bit(const unsigned char *buf, size_t len)
{
    if (len < 5) return 0;                /* Need at least 5 bytes */
    if (buf[0] < 0xB8 || buf[0] > 0xBF) return 0;  /* Not MOV reg,imm32 */
    /* Check that the 4-byte immediate is zero */
    if (buf[1] == 0 && buf[2] == 0 && buf[3] == 0 && buf[4] == 0)
        return buf[0] - 0xB8;  /* Return register number (0-7) */
    return -1;
}

/* The REX.W MOV $0, %r64 encoding:
 * REX.W (0x48) + 0xB8+reg + 8 zero bytes = 10 bytes total
 * Actually, the MOV r64, imm64 is encoded as:
 *   REX.W + 0xB8+reg + 8-byte immediate
 * Since REX.W is 0x48 (if no other extension needed). */
static int is_mov_zero_64bit(const unsigned char *buf, size_t len)
{
    if (len < 10) return 0;
    if (buf[0] != 0x48) return 0;         /* No REX.W prefix */
    if (buf[1] < 0xB8 || buf[1] > 0xBF) return 0;  /* Not MOV reg,imm64 */
    /* Check that the 8-byte immediate is zero */
    for (int i = 2; i < 10; i++)
        if (buf[i] != 0) return -1;
    return buf[1] - 0xB8;  /* Register number (0-7) */
}

/*
 * Also handle the case where it's just MOV $0, %r32 without REX
 * but we're targeting the 64-bit register anyway.  The semantics:
 *   MOV $0, %%eax  (5 bytes) zeros the upper 32 bits too (architectural
 *   behavior on x86-64: any 32-bit register write zero-extends to 64-bit).
 * So MOV $0, %%eax is equivalent to XOR RAX,RAX for zeroing!
 * But we'll still consider it for optimization.
 */

/*
 * --------------------------------------------------------------------
 * XOR register encoding table (32-bit version, 0x31 ModRM)
 *
 * Reg  Register   Encoding (ModRM byte)
 * 0    EAX        0x31 0xC0
 * 1    ECX        0x31 0xC9
 * 2    EDX        0x31 0xD2
 * 3    EBX        0x31 0xDB
 * 4    ESP        0x31 0xE4  (zeroing RSP is usually a bug!)
 * 5    EBP        0x31 0xED
 * 6    ESI        0x31 0xF6
 * 7    EDI        0x31 0xFF
 *
 * The ModRM byte = 0xC0 + (reg*8 + reg) = 0xC0 + (reg * 9)
 * Check: reg=0 -> 0xC0, reg=1 -> 0xC9, reg=2 -> 0xD2, etc.
 * ==================================================================== */
static unsigned char xor_modrm[8] = {
    0xC0, 0xC9, 0xD2, 0xDB, 0xE4, 0xED, 0xF6, 0xFF
};

/*
 * --------------------------------------------------------------------
 * Generate the optimized replacement sequence.
 *
 * For 32-bit case:
 *   0x31 0xModRM   (2 bytes)  -> XOR EAX,EAX etc.
 *   Then pad with 3-byte NOP (0x0F 0x1F 0x00) for equal total length.
 *
 * For 64-bit case:
 *   0x48 0x31 0xModRM  (3 bytes)  -> XOR RAX,RAX etc.
 *   Then pad with ... well, if the original was 10 bytes and we're
 *   replacing with 3 + 7 bytes of padding, that's a lot.  For
 *   demonstration we pad remaining with multi-byte NOPs.
 *
 * NOTE: In real code you'd just keep the shorter version (save bytes).
 * Padding is only shown here for the "preserve code size" scenario.
 * -------------------------------------------------------------------- */
static void generate_xor_sequence(int reg_num, int is_64bit,
                                  unsigned char *out, size_t *out_len)
{
    size_t pos = 0;

    if (is_64bit) {
        out[pos++] = 0x48;              /* REX.W prefix */
    }
    out[pos++] = 0x31;                  /* XOR r/m32/64, r32/64 */
    out[pos++] = xor_modrm[reg_num];    /* ModRM for reg^2 */

    *out_len = pos;
}

/*
 * --------------------------------------------------------------------
 * Scan a buffer for MOV $0, %reg sequences and suggest replacements.
 * -------------------------------------------------------------------- */
static int scan_and_replace(unsigned char *buf, size_t len)
{
    int replacements = 0;
    size_t i = 0;

    while (i < len) {
        /* Try 64-bit first (10 bytes) */
        int reg = is_mov_zero_64bit(buf + i, len - i);
        if (reg >= 0 && reg <= 7) {
            printf("  Offset %4zu: MOV $0, %%r%d   (10 bytes: 48 %02X 00 00 00 00 00 00 00 00 00)\n",
                   i, reg, 0xB8 + reg);
            printf("              -> XOR %%r%d, %%r%d  (3 bytes: 48 31 %02X)\n",
                   reg, reg, xor_modrm[reg]);
            printf("              => Saves 7 bytes! (no padding needed in practice)\n");

            /* Actually replace it */
            unsigned char replacement[10];
            size_t rlen = 0;
            generate_xor_sequence(reg, 1, replacement, &rlen);

            /* For 10 -> 3, we'd pad with NOPs if preserving length.
             * We show both approaches. */
            printf("                 Unpadded (optimal): ");
            for (size_t j = 0; j < rlen; j++)
                printf("%02X ", replacement[j]);
            printf("\n");

            /* Pad to original 10 bytes with multi-byte NOPs */
            size_t pad = 10 - rlen;
            memcpy(buf + i, replacement, rlen);
            /* Pad remaining with multi-byte NOPs */
            size_t pos = i + rlen;
            while (pad > 0) {
                if (pad >= 8) {
                    unsigned char nop8[] = {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
                    memcpy(buf + pos, nop8, 8);
                    pos += 8; pad -= 8;
                } else if (pad >= 7) {
                    unsigned char nop7[] = {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00};
                    memcpy(buf + pos, nop7, 7);
                    pos += 7; pad -= 7;
                } else if (pad >= 5) {
                    unsigned char nop5[] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
                    memcpy(buf + pos, nop5, 5);
                    pos += 5; pad -= 5;
                } else if (pad >= 3) {
                    unsigned char nop3[] = {0x0F, 0x1F, 0x00};
                    memcpy(buf + pos, nop3, 3);
                    pos += 3; pad -= 3;
                } else if (pad >= 2) {
                    unsigned char nop2[] = {0x66, 0x90};
                    memcpy(buf + pos, nop2, 2);
                    pos += 2; pad -= 2;
                } else {
                    buf[pos++] = 0x90; pad--;
                }
            }

            printf("                 Padded version:    ");
            for (size_t j = 0; j < 10; j++)
                printf("%02X ", buf[i + j]);
            printf("\n\n");

            i += 10;
            replacements++;
            continue;
        }

        /* Try 32-bit (5 bytes) */
        reg = is_mov_zero_32bit(buf + i, len - i);
        if (reg >= 0 && reg <= 7) {
            printf("  Offset %4zu: MOV $0, %%e%d  (5 bytes: %02X 00 00 00 00)\n",
                   i,
                   reg, 0xB8 + reg);
            printf("              -> XOR %%e%d, %%e%d (2 bytes: 31 %02X)\n",
                   reg, reg, xor_modrm[reg]);
            printf("              => Saves 3 bytes!\n");

            unsigned char replacement[5];
            size_t rlen = 0;
            generate_xor_sequence(reg, 0, replacement, &rlen);

            printf("                 Unpadded: ");
            for (size_t j = 0; j < rlen; j++)
                printf("%02X ", replacement[j]);
            printf("\n");

            /* Pad to 5 bytes */
            size_t pad = 5 - rlen;
            memcpy(buf + i, replacement, rlen);
            unsigned char nop3[] = {0x0F, 0x1F, 0x00};
            if (pad > 0)
                memcpy(buf + i + rlen, nop3, pad);

            printf("                 Padded:   ");
            for (size_t j = 0; j < 5; j++)
                printf("%02X ", buf[i + j]);
            printf("\n\n");

            i += 5;
            replacements++;
            continue;
        }

        i++;
    }

    return replacements;
}

/*
 * ====================================================================
 * MAIN
 * ==================================================================== */
int main(void)
{
    printf("============================================================\n");
    printf("  ZEROING IDIOM OPTIMIZATION\n");
    printf("============================================================\n\n");

    printf("Inefficient pattern:  MOV $0, %%reg\n");
    printf("                     (opcodes 0xB8-0xBF + 4/8 zero bytes)\n");
    printf("Optimal pattern:      XOR %%reg, %%reg\n");
    printf("                     (opcode 0x31 + ModRM = 2 bytes, or 3 with REX.W)\n\n");

    /*
     * Build a test buffer containing various MOV $0 patterns
     */
    unsigned char buf[256];
    memset(buf, 0xCC, sizeof(buf));  /* Fill with INT3 as don't-care */
    size_t pos = 0;

    /* MOV $0, %%eax (5 bytes) */
    buf[pos++] = 0xB8;  /* MOV EAX, imm32 */
    buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00;

    /* MOV $0, %%ecx (5 bytes) */
    buf[pos++] = 0xB9;
    buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00;

    /* MOV $0, %rdx (10 bytes: REX.W + 0xBA + 8 zero bytes) */
    buf[pos++] = 0x48;  /* REX.W */
    buf[pos++] = 0xBA;  /* MOV RDX, imm64 */
    buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00;
    buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00;

    /* MOV $0, %rbx (10 bytes) */
    buf[pos++] = 0x48;
    buf[pos++] = 0xBB;
    buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00;
    buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00;

    /* MOV $0, %%esi (5 bytes) */
    buf[pos++] = 0xBE;
    buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00;

    size_t buf_len = pos;

    printf("Original buffer (%zu bytes):\n", buf_len);
    for (size_t i = 0; i < buf_len; i++)
        printf("%02X ", buf[i]);
    printf("\n\n");

    printf("=== Scanning and Replacing ===\n\n");
    int count = scan_and_replace(buf, buf_len);

    printf("=== Summary ===\n");
    printf("  Found and replaced %d MOV $0 patterns.\n\n", count);

    printf("Optimized buffer:\n");
    for (size_t i = 0; i < buf_len; i++)
        printf("%02X ", buf[i]);
    printf("\n\n");

    printf("============================================================\n");
    printf("  WHY XOR IS BETTER THAN MOV $0\n");
    printf("============================================================\n\n");

    printf("1. REGISTER RENAMING OPTIMIZATION\n");
    printf("   Modern CPUs detect the XOR R,R idiom (or SUB R,R) and\n");
    printf("   handle it at the renaming stage:\n");
    printf("   - Instead of executing the XOR in the ALU, the CPU simply\n");
    printf("     maps the architectural register to a new physical\n");
    printf("     register that is pre-initialized to zero.\n");
    printf("   - This costs ZERO µops on modern microarchitectures.\n");
    printf("   - The instruction 'disappears' from the execution pipeline.\n");
    printf("   - First seen in Intel P6 (Pentium Pro, 1995).\n\n");

    printf("2. BREAKS DEPENDENCY CHAINS\n");
    printf("   MOV $0, %%eax has a data dependency on EAX (the CPU must\n");
    printf("   wait for the previous EAX writer to finish before MOV).\n");
    printf("   XOR %%eax, %%eax explicitly BREAKS that dependency because\n");
    printf("   the dependency-tracking logic recognizes it as independent.\n\n");

    printf("3. SMALLER CODE SIZE\n");
    printf("   - MOV $0, %%eax:  5 bytes (B8 00 00 00 00)\n");
    printf("   - XOR %%eax,%%eax: 2 bytes (31 C0)\n");
    printf("   - 60%% reduction!  Less icache pressure.\n\n");

    printf("4. FEWER UOPS\n");
    printf("   - MOV $0, %%eax:  1 µop (load immediate from instruction stream)\n");
    printf("   - XOR %%eax,%%eax: 0 µop (handled at rename, no execution)\n\n");

    printf("5. NO EXECUTION PORT CONTENTION\n");
    printf("   The MOV requires a port (typically port 0/1/5 for ALU or\n");
    printf("   port 1 for MOV elimination on Intel).  XOR R,R uses NO port.\n\n");

    printf("COMPILER BEHAVIOR:\n");
    printf("  GCC and Clang always use XOR for zeroing since ~1995.\n");
    printf("  MSVC also uses XOR since ~2005.\n");
    printf("  Hand-written assembly or old binaries may still have MOV $0.\n\n");

    printf("SIDE NOTE: SUB REG,REG is also recognized:\n");
    printf("  SUB EAX,EAX (opcodes: 0x29 0xC0) has a similar zero µop\n");
    printf("  cost but uses a different opcode encoding.  XOR is preferred\n");
    printf("  by convention but both work identically.\n\n");

    printf("WARNING:\n");
    printf("  XOR ESP,ESP (0x31 0xE4) zeroes the stack pointer!\n");
    printf("  This should NEVER be used except in very early kernel init.\n");
    printf("  The CPU will fault on the next push/pop/call/ret.\n");

    return 0;
}
