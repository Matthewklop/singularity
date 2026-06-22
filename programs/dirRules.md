# Directory Rules for /home/u/criticalCodeSnippetsForOracle/

## Purpose
This directory contains standalone, zero-dependency code snippets.
Each file is self-contained — compilable with just gcc, no external refs.

## Rules

### 1. Zero Dependencies
- No includes beyond standard C headers
- Compiles with: `gcc -O3 -o output input.c -lm`
- Runs with: `./output [args]`

### 2. Zero Pre-Existing Code References
- No "see other file"
- No "this type is defined elsewhere"
- EVERYTHING needed is IN the snippet

### 3. CRITICAL: Comment Everything
Every snippet MUST be HEAVILY commented. Assume the reader has NEVER seen
this technique before. Explain:

- WHAT each block of code does at a high level
- WHY this technique works (the underlying mechanism)
- HOW the hardware/OS/kernel makes this possible
- WHAT the hex opcodes mean (for assembly-level snippets)
- WHAT could go wrong and why
- WHAT each argument/parameter does

Bad comment:  `// Write to memory`
Good comment: `// Write to target process memory using process_vm_writev.
//  This syscall lets us write directly to another process's address space
//  without ptrace. It uses the remote process's page tables directly.
//  Requires: same UID or root, and target memory must be mapped.
//  Returns: number of bytes written, or -1 with errno set.
//  errno=13 (EACCES) = permission denied, try sudo.
//  errno=14 (EFAULT) = bad address, target page not mapped.`

### 4. Self-Contained
- Must work on a brand new machine with only gcc
- Include full header block at top

### 5. Naming Convention
- `NNN_description.c`
- NNN = 001, 002...
- description = short snake_case

### 6. Categories
- 000-099: Timing & Performance
- 100-199: Binary Patching
- 200-299: Memory & Process
- 300-399: CPU Architecture
- 400-499: SIMD & Vectorization
- 500-599: System Programming
- 600-699: Network & IPC
- 700-799: Utility & Debug

### 7. Each file starts with:
```
// ============================================================
// CRITICAL SNIPPET
// NAME: short_name
// CATEGORY: XXX-XXX Category Name
// WHAT: One-line description of what this does
// WHY: One-line explanation of the underlying mechanism
// BUILD: gcc -O3 -o output input.c -lm
// RUN: ./output [args]
// DEPENDS: none
// ============================================================
```

### 8. Frozen once written
Create new version with higher number for improvements.

### 9. One technique per file
