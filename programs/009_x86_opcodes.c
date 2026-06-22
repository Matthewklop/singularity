/*
 * ====================================================================
 * CRITICAL SNIPPET: 009_x86_opcodes.c
 * ====================================================================
 * Tables of x86-64 opcodes with a decode function.
 *
 * This file provides a comprehensive reference for common x86-64
 * instruction encodings, along with a disassembler-like decode
 * function that parses raw bytes and identifies instructions.
 *
 * x86-64 INSTRUCTION ENCODING OVERVIEW:
 *   Instructions are variable-length (1 to 15 bytes).  The general
 *   format is:
 *
 *     [Prefixes] [REX] [Opcode] [ModRM] [SIB] [Displacement] [Immediate]
 *
 *   PREFIXES (0-4 bytes):
 *     0x66 – Operand-size override (16-bit vs 32-bit)
 *     0xF0 – LOCK prefix
 *     0xF2 – REPNE/REPNZ
 *     0xF3 – REP/REPE/REPZ
 *     0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65 – Segment overrides
 *
 *   REX PREFIX (0x48-0x4F):
 *     REX prefixes extend the x86-64 instruction set to use 64-bit
 *     operands and the extended registers (r8-r15).  Format:
 *       0b0100WRXB
 *       W = 0 -> 32-bit operand, 1 -> 64-bit operand
 *       R = extends ModRM.reg field (4th bit)
 *       X = extends SIB index field (4th bit)
 *       B = extends ModRM.rm or SIB.base (4th bit)
 *
 *   ModRM BYTE:  [mod:2][reg:3][r/m:3]
 *     mod (2 bits): 00 = register indirect, 01 = disp8, 10 = disp32,
 *                   11 = register direct
 *     reg (3 bits):  opcode extension or register operand
 *     r/m (3 bits):  the other register or address mode
 *
 *   SIB BYTE: [scale:2][index:3][base:3]
 *     Used when ModRM.r/m = 100 (SP-relative addressing).
 *     Address = base + index * (1 << scale)
 *
 * COMPILE:
 *   gcc -O2 -Wall -o 009_x86_opcodes 009_x86_opcodes.c
 *
 * USAGE:
 *   ./009_x86_opcodes [hex_bytes...]
 *   Example: ./009_x86_opcodes 48 8b 05 00 00 00 00
 *            (which is: mov rax, [rip+disp32])
 *   Without arguments, prints a reference table of all opcodes.
 * ==================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

/*
 * ====================================================================
 *  x86-64 OPCODE REFERENCE TABLES
 * ====================================================================
 *
 * Each entry describes a class of instructions.  We'll define a struct
 * that maps opcode bytes to a human-readable mnemonic.
 * ==================================================================== */

/* Maximum instruction bytes we'll try to decode */
#define MAX_INSN_BYTES 15

/* Registers indexed by ModRM.rm (or reg field), with and without REX.B */
static const char *reg_names[16] = {
    /* 0-7  (no REX)           8-15 (with REX.B) */
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"
};

/*
 * ====================================================================
 * ModRM byte decoder
 *
 * The ModRM byte (Modifier-Register-Mode) encodes the addressing mode
 * for most x86 instructions.  Breakdown:
 *
 *   Bits 7-6 (MOD):   Addressing mode
 *     00 – Memory, no displacement (unless r/m=101 → RIP-relative)
 *     01 – Memory + disp8  (8-bit signed displacement)
 *     10 – Memory + disp32 (32-bit signed displacement)
 *     11 – Register direct (no memory access)
 *
 *   Bits 5-3 (REG):    Register operand or opcode extension
 *   Bits 2-0 (RM):     Register or memory operand
 *
 * Table of RM for MOD != 11 (memory addressing modes):
 *   RM  |  Default           | With SIB
 *   000 | [rax]               |
 *   001 | [rcx]               |
 *   010 | [rdx]               |
 *   011 | [rbx]               |
 *   100 | SIB follows         | [base + index*scale]
 *   101 | [rip+disp32] (MOD=00)| [base + disp32] (MOD=01/10)
 *   110 | [rsi]               |
 *   111 | [rdi]               |
 *
 * For MOD == 11 (register mode):
 *   000 = al/ax/eax/rax    100 = ah/sp/esp/rsp
 *   001 = cl/cx/ecx/rcx    101 = ch/bp/ebp/rbp
 *   010 = dl/dx/edx/rdx    110 = dh/si/esi/rsi
 *   011 = bl/bx/ebx/rbx    111 = bh/di/edi/rdi
 *   (With REX/R → extended registers r8-r15)
 * ==================================================================== */
static const char *modrm_mod_desc[] = {
    [0] = "[reg]          (no displacement)",
    [1] = "[reg + disp8]  (8-bit displacement)",
    [2] = "[reg + disp32] (32-bit displacement)",
    [3] = "register       (direct register operand)",
};

/* Address mode descriptions for memory forms */
static const char *modrm_rm_mem_desc[8] = {
    "[rax]",           "[rcx]",           "[rdx]",           "[rbx]",
    "SIB follows",     "[rip+disp32]",    "[rsi]",           "[rdi]",
};

/* Register names for MOD==11 (direct register) */
static const char *modrm_rm_reg_desc[8] = {
    "%rax/%eax/%ax/%al", "%rcx/%ecx/%cx/%cl",
    "%rdx/%edx/%dx/%dl", "%rbx/%ebx/%bx/%bl",
    "%rsp/%esp/%sp/%ah", "%rbp/%ebp/%bp/%ch",
    "%rsi/%esi/%si/%dh", "%rdi/%edi/%di/%bh",
};

/*
 * ====================================================================
 * Helper: decode one instruction and return the mnemonic string
 * (static buffer).  This is a simplified disassembler.
 * ==================================================================== */
typedef struct {
    const char *mnemonic;
    int         length;     /* Total instruction length in bytes */
    char        detail[128]; /* Additional decode info */
} decoded_insn_t;

/*
 * decode_instruction
 *
 * Given a buffer of raw bytes, attempts to decode the first
 * instruction and return its mnemonic and length.
 *
 * This is a HIGHLY SIMPLIFIED decoder covering only the specified
 * opcodes.  A full disassembler would handle hundreds more opcodes,
 * prefixes, VEX/EVEX encodings, etc.
 */
static decoded_insn_t decode_instruction(const unsigned char *bytes)
{
    decoded_insn_t result = { "unknown", 1, "" };

    if (!bytes)
        return result;

    /*
     * Check for REX prefix (0x48-0x4F).
     * REX is a single-byte prefix that modifies the following opcode.
     * 0x48 = REX.W (64-bit operand size)
     * 0x49 = REX.WB, 0x4A = REX.WX, 0x4B = REX.WXB
     * 0x4C = REX.R,  0x4D = REX.RB, 0x4E = REX.RX, 0x4F = REX.RXB
     */
    int has_rex = 0;
    int rex_byte = 0;
    int idx = 0;

    if (bytes[idx] >= 0x48 && bytes[idx] <= 0x4F) {
        has_rex = 1;
        rex_byte = bytes[idx];
        idx++;
    }

    /*
     * Now decode based on the opcode at bytes[idx].
     * We'll handle the specific opcodes listed in the spec.
     */
    unsigned char op = bytes[idx];

    /*
     * NOP variants
     */
    if (op == 0x90 && !has_rex) {
        result.mnemonic = "NOP (XCHG EAX,EAX)";
        result.length = idx + 1;
        snprintf(result.detail, sizeof(result.detail), "1-byte NOP");
        return result;
    }

    /*
     * 0x0F two-byte opcode prefix
     * Many instructions start with 0x0F as an escape.
     */
    if (op == 0x0F && idx + 1 < MAX_INSN_BYTES) {
        unsigned char op2 = bytes[idx + 1];

        /* NOP variants: 0F 1F /0 */
        if (op2 == 0x1F) {
            int nop_len = 2; /* 0F 1F */
            /* Need ModRM byte for /0 encoding */
            if (idx + 2 < MAX_INSN_BYTES) {
                nop_len = 3; /* + ModRM */
                unsigned char modrm = bytes[idx + 2];
                int mod = (modrm >> 6) & 0x3;
                int rm  = modrm & 0x7;
                if (mod == 0 && rm == 0) {
                    snprintf(result.detail, sizeof(result.detail),
                             "3-byte NOP (0F 1F 00)");
                    result.length = idx + 3;
                } else if (mod == 1 && rm == 0) {
                    /* 0F 1F 40 xx : 4-byte NOP with disp8 */
                    snprintf(result.detail, sizeof(result.detail),
                             "4-byte NOP (disp8)");
                    result.length = idx + 4;
                } else if (mod == 1 && rm == 4) {
                    /* 0F 1F 44 xx xx : 5-byte NOP with SIB */
                    snprintf(result.detail, sizeof(result.detail),
                             "5-byte NOP (SIB+disp8)");
                    result.length = idx + 5;
                } else if (mod == 2 && rm == 0) {
                    /* 0F 1F 80 xx xx xx xx : 7-byte NOP with disp32 */
                    snprintf(result.detail, sizeof(result.detail),
                             "7-byte NOP (disp32)");
                    result.length = idx + 7;
                } else if (mod == 2 && rm == 4) {
                    /* 0F 1F 84 xx xx xx xx xx : 8-byte NOP */
                    snprintf(result.detail, sizeof(result.detail),
                             "8-byte NOP (SIB+disp32)");
                    result.length = idx + 8;
                }
                result.mnemonic = "NOP (multi-byte)";
            }
            return result;
        }

        /* Conditional jumps near: 0F 80-8F + rel32 */
        if (op2 >= 0x80 && op2 <= 0x8F) {
            static const char *cc_names[16] = {
                "O",  "B/NAE/C", "Z/E", "BE/NA",
                "S",  "NS",      "P/PE","NP/PO",
                "L/NGE","NL/GE","LE/NG","NLE/G",
                "??", "??",      "??",   "??"
            };
            int cc = op2 & 0x0F;
            snprintf(result.detail, sizeof(result.detail),
                     "Conditional jump near: J%s rel32", cc_names[cc]);
            result.mnemonic = "Jcc near";
            result.length = idx + 6; /* 0F + op2 + 4-byte rel32 */
            return result;
        }

        /* CMOVcc: 0F 40-4F + ModRM */
        if (op2 >= 0x40 && op2 <= 0x4F) {
            static const char *cmov_cc[16] = {
                "O",  "B/NAE/C", "Z/E", "BE/NA",
                "S",  "NS",      "P/PE","NP/PO",
                "L/NGE","NL/GE","LE/NG","NLE/G",
                "??", "??",      "??",   "??"
            };
            int cc = op2 & 0x0F;
            snprintf(result.detail, sizeof(result.detail),
                     "CMOV%s", cmov_cc[cc]);
            result.mnemonic = "CMOVcc";
            result.length = idx + 3; /* 0F + op2 + ModRM */
            return result;
        }

        /* PREFETCHT0: 0F 18 /0 (ModRM reg=0) */
        if (op2 == 0x18 && idx + 2 < MAX_INSN_BYTES) {
            unsigned char modrm = bytes[idx + 2];
            int reg = (modrm >> 3) & 0x7;
            if (reg == 0) {
                result.mnemonic = "PREFETCHT0";
                result.length = idx + 3;
                snprintf(result.detail, sizeof(result.detail),
                         "Prefetch to all cache levels");
                return result;
            }
        }

        result.mnemonic = "0x0F-prefixed";
        result.length = idx + 2;
        return result;
    }

    /*
     * RET (near return): 0xC3
     */
    if (op == 0xC3 && !has_rex) {
        result.mnemonic = "RET (near return)";
        result.length = idx + 1;
        snprintf(result.detail, sizeof(result.detail), "Return to caller");
        return result;
    }

    /*
     * JMP short: 0xEB + rel8
     */
    if (op == 0xEB) {
        result.mnemonic = "JMP short";
        result.length = idx + 2;
        snprintf(result.detail, sizeof(result.detail), "Jump with 8-bit offset");
        return result;
    }

    /*
     * JMP near: 0xE9 + rel32
     */
    if (op == 0xE9) {
        result.mnemonic = "JMP near";
        result.length = idx + 5;
        snprintf(result.detail, sizeof(result.detail), "Jump with 32-bit offset");
        return result;
    }

    /*
     * Conditional jumps short: 0x70-0x7F + rel8
     */
    if (op >= 0x70 && op <= 0x7F) {
        static const char *cc_short[16] = {
            "O",  "B/NAE/C", "Z/E", "BE/NA",
            "S",  "NS",      "P/PE","NP/PO",
            "L/NGE","NL/GE","LE/NG","NLE/G",
            "??", "??",      "??",   "??"
        };
        int cc = op & 0x0F;
        result.mnemonic = "Jcc short";
        result.length = idx + 2;
        snprintf(result.detail, sizeof(result.detail),
                 "Conditional jump short: J%s rel8", cc_short[cc]);
        return result;
    }

    /*
     * MOV variants:
     *   0x88 – MOV r/m8, r8   (move byte register to memory/register)
     *   0x89 – MOV r/m32/64, r32/64
     *   0x8A – MOV r8, r/m8
     *   0x8B – MOV r32/64, r/m32/64
     */
    if (op == 0x88 || op == 0x89 || op == 0x8A || op == 0x8B) {
        const char *size = (op == 0x88 || op == 0x8A) ? "byte" : "32/64-bit";
        const char *dir  = (op == 0x88 || op == 0x89) ? "reg->r/m" : "r/m->reg";
        snprintf(result.detail, sizeof(result.detail),
                 "MOV %s (%s)", size, dir);
        result.mnemonic = "MOV";
        /* Need ModRM for full length */
        result.length = idx + 2 + (has_rex ? 0 : 0);
        /* Minimum is 2 (opcode + ModRM) */
        if (idx + 1 < MAX_INSN_BYTES) {
            result.length = idx + 2;
            unsigned char modrm = bytes[idx + 1];
            int mod = (modrm >> 6) & 0x3;
            if (mod == 1) result.length += 1;  /* +disp8 */
            if (mod == 2) result.length += 4;  /* +disp32 */
            if ((modrm & 7) == 4) result.length += 1;  /* +SIB */
        }
        return result;
    }

    /*
     * REP STOSB: 0xF3 0xAA
     * REP prefix (0xF3) + STOSB (0xAA)
     * Writes AL to [rdi], increments/decrements rdi.
     */
    if (op == 0xF3 && bytes[idx+1] == 0xAA) {
        result.mnemonic = "REP STOSB";
        result.length = idx + 2;
        snprintf(result.detail, sizeof(result.detail),
                 "Fill buffer with AL value");
        return result;
    }

    /*
     * REP prefix with other string ops:
     *   0xF3 0xA4 – REP MOVSB
     *   0xF3 0xAB – REP STOSD/STOSQ
     *   0xF3 0x6C – REP INSB (privileged)
     * etc.
     */
    if (op == 0xF3) {
        result.mnemonic = "REP prefixed";
        result.length = idx + 2;
        snprintf(result.detail, sizeof(result.detail),
                 "REP prefix (0xF3) + opcode 0x%02X", bytes[idx+1]);
        return result;
    }

    /* Fallback */
    if (has_rex) {
        snprintf(result.detail, sizeof(result.detail),
                 "REX=0x%02X opcode=0x%02X", rex_byte, op);
        result.mnemonic = "opcode_with_rex";
    } else {
        snprintf(result.detail, sizeof(result.detail),
                 "single-byte opcode: 0x%02X", op);
    }
    result.length = idx + 1;

    return result;
}

/*
 * ====================================================================
 * Reference printing: MODRM table
 * ==================================================================== */
static void print_modrm_table(void)
{
    printf("\n=== ModRM Byte Decoding ===\n");
    printf("ModRM = [mod:2][reg:3][r/m:3]\n\n");

    printf("MOD field (bits 7-6):\n");
    for (int m = 0; m < 4; m++) {
        printf("  %d%d = %s\n", (m>>1)&1, m&1, modrm_mod_desc[m]);
    }

    printf("\nR/M field (bits 2-0) for memory addressing (MOD != 11):\n");
    for (int rm = 0; rm < 8; rm++) {
        printf("  %d%d%d = %s\n",
               (rm>>2)&1, (rm>>1)&1, rm&1,
               modrm_rm_mem_desc[rm]);
    }

    printf("\nR/M field for register direct (MOD == 11):\n");
    for (int rm = 0; rm < 8; rm++) {
        printf("  %d%d%d = %s\n",
               (rm>>2)&1, (rm>>1)&1, rm&1,
               modrm_rm_reg_desc[rm]);
    }

    printf("\n=== Condition Codes for Jcc / CMOVcc ===\n");
    printf("CC  Mnemonic  Description\n");
    printf("--  --------  ----------------------------------------\n");
    printf(" 0  JO        Overflow\n");
    printf(" 1  JB/JNAE/JC  Below / Not Above or Equal / Carry\n");
    printf(" 2  JZ/JE     Zero / Equal\n");
    printf(" 3  JBE/JNA   Below or Equal / Not Above\n");
    printf(" 4  JS        Sign (negative)\n");
    printf(" 5  JNS       Not Sign (non-negative)\n");
    printf(" 6  JP/JPE    Parity / Parity Even\n");
    printf(" 7  JNP/JPO   Not Parity / Parity Odd\n");
    printf(" 8  JL/JNGE   Less / Not Greater or Equal\n");
    printf(" 9  JNL/JGE   Not Less / Greater or Equal\n");
    printf(" A  JLE/JNG   Less or Equal / Not Greater\n");
    printf(" B  JNLE/JG   Not Less or Equal / Greater\n");
}

/*
 * ====================================================================
 * MAIN
 * ==================================================================== */
int main(int argc, char *argv[])
{
    if (argc < 2) {
        /* No arguments: print reference tables */
        printf("============================================================\n");
        printf("  x86-64 OPCODE REFERENCE\n");
        printf("============================================================\n\n");

        printf("=== REX Prefix (0x48-0x4F) ===\n");
        printf("Bit pattern: 0100 WRXB\n");
        printf("  W (bit 3): 0=32-bit operand, 1=64-bit operand\n");
        printf("  R (bit 2): Extends ModRM.reg (4th bit for r8-r15)\n");
        printf("  X (bit 1): Extends SIB index (4th bit for r8-r15)\n");
        printf("  B (bit 0): Extends ModRM.rm or SIB base\n\n");
        printf("  REX byte breakdown:\n");
        printf("    0x48 (0100 1000) = REX.W         (64-bit, no extension)\n");
        printf("    0x49 (0100 1001) = REX.WB        (64-bit, extend rm)\n");
        printf("    0x4A (0100 1010) = REX.WX        (64-bit, extend index)\n");
        printf("    0x4B (0100 1011) = REX.WXB       (64-bit, extend both)\n");
        printf("    0x4C (0100 1100) = REX.R         (32-bit, extend reg)\n");
        printf("    0x4D (0100 1101) = REX.RB        (32-bit, extend reg+rm)\n");
        printf("    0x4E (0100 1110) = REX.RX        (32-bit, extend reg+index)\n");
        printf("    0x4F (0100 1111) = REX.RXB       (32-bit, extend all)\n\n");

        printf("=== Single-Byte Opcodes ===\n");
        printf("  0x90 – NOP (XCHG EAX,EAX)\n");
        printf("  0xC3 – RET (near return)\n");
        printf("  0xEB + rel8 – JMP short\n");
        printf("  0xE9 + rel32 – JMP near\n");
        printf("  0x70-0x7F + rel8  – Jcc short (conditional jump short)\n");
        printf("  0x88 – MOV r/m8, r8\n");
        printf("  0x89 – MOV r/m32/64, r32/64\n");
        printf("  0x8A – MOV r8, r/m8\n");
        printf("  0x8B – MOV r32/64, r/m32/64\n");
        printf("  0xB8-0xBF + imm32 – MOV r32, imm32 (or MOV r64, imm64)\n");
        printf("  0x05 + rel32 – ADD EAX, imm32\n");
        printf("  0x2D + rel32 – SUB EAX, imm32\n\n");

        printf("=== Two-Byte Opcodes (0x0F prefix) ===\n");
        printf("  0x0F 0x1F /0     – Multi-byte NOP variants\n");
        printf("  0x0F 0x80-0x8F + rel32 – Jcc near (conditional jump near)\n");
        printf("  0x0F 0x40-0x4F + ModRM – CMOVcc\n");
        printf("  0x0F 0x18 /0     – PREFETCHT0\n\n");

        printf("=== Prefix-Based ===\n");
        printf("  0xF3 0xAA – REP STOSB (fill buffer with AL)\n");
        printf("  0xF3 0xA4 – REP MOVSB (copy buffer)\n");
        printf("  0xF3 0xAB – REP STOSD/STOSQ\n");
        printf("  0x66 0x90 – 2-byte NOP\n");
        printf("  0x66 0x0F 0x1F 0x44 0x00 0x00 – 6-byte NOP\n\n");

        print_modrm_table();

        printf("\n=== Decoding Demos ===\n");
        printf("Run with hex bytes to decode: %s <hex bytes...>\n", argv[0]);
        printf("Example: %s 48 8b 05 00 00 00 00\n", argv[0]);
        printf("         (mov rax, [rip+disp32])\n");

        /* Demo some instructions */
        printf("\n--- Built-in Decode Tests ---\n");
        unsigned char test_insns[][8] = {
            {0x90},                         /* NOP */
            {0xC3},                         /* RET */
            {0xEB, 0x05},                   /* JMP short +5 */
            {0xE9, 0x00, 0x00, 0x00, 0x00}, /* JMP near 0 */
            {0x74, 0x03},                   /* JZ short +3 */
            {0x0F, 0x84, 0x00, 0x00, 0x00, 0x00}, /* JZ near 0 */
            {0x0F, 0x44, 0xC8},             /* CMOVE rax, r8 (ModRM) */
            {0x0F, 0x1F, 0x00},             /* 3-byte NOP */
            {0x88, 0xD8},                   /* MOV AL, BL */
            {0x89, 0xD8},                   /* MOV EAX, EBX */
            {0xF3, 0xAA},                   /* REP STOSB */
            {0x0F, 0x18, 0x08},             /* PREFETCHT0 [rax] */
            {0x48, 0x89, 0xD8},             /* MOV RAX, RBX (with REX.W) */
        };
        int num_tests = sizeof(test_insns) / sizeof(test_insns[0]);
        for (int i = 0; i < num_tests; i++) {
            decoded_insn_t d = decode_instruction(test_insns[i]);
            printf("  ");
            for (int j = 0; j < d.length && j < 8; j++)
                printf("%02X ", test_insns[i][j]);
            for (int j = d.length; j < 8; j++) printf("   ");
            printf("  -> %-20s  %s\n", d.mnemonic, d.detail);
        }

        return 0;
    }

    /* User provided hex bytes: decode them */
    printf("Decoding: ");
    unsigned char bytes[MAX_INSN_BYTES];
    int num_bytes = 0;
    for (int i = 1; i < argc && num_bytes < MAX_INSN_BYTES; i++) {
        unsigned long b = strtoul(argv[i], NULL, 16);
        bytes[num_bytes++] = (unsigned char)(b & 0xFF);
        printf("%02X ", (unsigned char)b);
    }
    printf("\n");

    decoded_insn_t d = decode_instruction(bytes);
    printf("  %s: %s\n", d.mnemonic, d.detail);
    printf("  Instruction length: %d bytes\n", d.length);

    return 0;
}
