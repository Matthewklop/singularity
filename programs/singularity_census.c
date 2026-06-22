/*
 * SINGULARITY CENSUS — Generate ALL program combinations.
 * Pre-compile once. Benchmark each via function call.
 * Every program that can exist, does exist.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char *seeds[] = {
    "s += r(0x19C) & 0xFF;",
    "s += r(0x198) & 0xFF;",
    "s += r(0x611) & 0xFF;",
    "s += r(0xE8) & 0xFF;",
    "s += c * 7;",
    "s += c * 13;",
    "s += c & 0xFF;",
    "s += (c >> 8) & 0xFF;",
    "s += (c * 7 + 13) & 0xFF;",
    "s += c % 32;",
    "asm volatile(\"clflush (%0)\" : : \"r\"(&s) : \"memory\");",
    "asm volatile(\"mfence\" : : : \"memory\");",
    "asm volatile(\"pause\" : : : \"memory\");",
    "{ uint64_t cr; asm volatile(\"mov %%cr0, %0\" : \"=r\"(cr)); s += cr; }",
    "{ uint32_t lo; asm volatile(\"rdpmc\" : \"=a\"(lo) : \"c\"(0) : \"edx\"); s += lo; }",
    "static int at[32]; at[c & 31]++; s += at[c & 31];",
    "static uint64_t fab[256]; fab[c & 255]++; s += fab[c & 255];",
    "s ^= s >> 12; s ^= s << 25; s ^= s >> 27;",
};
static int n_seeds = sizeof(seeds)/sizeof(seeds[0]);

int main() {
    int total = 0;
    int n_stmts = 4;
    int indices[4];
    
    // Count combinations first
    for(int a = 0; a < n_seeds; a++)
        for(int b = 0; b < n_seeds; b++)
            for(int c = 0; c < n_seeds; c++)
                for(int d = 0; d < n_seeds; d++)
                    total++;
    
    printf("CENSUS — generating all %d programs\n", total);
    printf("Seeds: %d, Statements per program: %d\n\n", n_seeds, n_stmts);
    
    // Generate single .c file with all programs
    char fn[] = "/tmp/_census.c";
    FILE *f = fopen(fn, "w");
    if(!f) return 1;
    
    fprintf(f, "#define _GNU_SOURCE\n#include <stdio.h>\n#include <stdint.h>\n#include <fcntl.h>\n#include <unistd.h>\n\n");
    fprintf(f, "static int m; static void i(void){m=open(\"/dev/cpu/0/msr\",O_RDONLY);}\n");
    fprintf(f, "static uint64_t r(uint32_t a){uint64_t v;pread(m,&v,8,a);return v;}\n\n");
    
    int idx = 0;
    for(int a = 0; a < n_seeds; a++)
        for(int b = 0; b < n_seeds; b++)
            for(int c = 0; c < n_seeds; c++)
                for(int d = 0; d < n_seeds; d++) {
                    fprintf(f, "static uint64_t p%d(void) {\n", idx);
                    fprintf(f, "    volatile uint64_t s = 0;\n");
                    fprintf(f, "    for(int c=0;c<100000;c++){\n");
                    fprintf(f, "        %s\n        %s\n        %s\n        %s\n", 
                            seeds[a], seeds[b], seeds[c], seeds[d]);
                    fprintf(f, "        asm volatile(\"\": : :\"memory\");\n");
                    fprintf(f, "    }\n    return s;\n}\n");
                    idx++;
                }
    
    fprintf(f, "\nint main() {\n    i();\n");
    fprintf(f, "    uint64_t best = ~0ULL;\n    int best_i = -1;\n");
    fprintf(f, "    printf(\"Benchmarking %%d programs...\\n\", %d);\n", total);
    fprintf(f, "    for(int i = 0; i < %d; i++) {\n", total);
    fprintf(f, "        uint32_t l,h; asm volatile(\"rdtsc\":\"=a\"(l),\"=d\"(h)); uint64_t t0=((uint64_t)h<<32)|l;\n");
    fprintf(f, "        volatile uint64_t r = p0(); (void)r;\n");
    fprintf(f, "        asm volatile(\"rdtsc\":\"=a\"(l),\"=d\"(h)); uint64_t t1=((uint64_t)h<<32)|l;\n");
    fprintf(f, "        uint64_t cyc = t1 - t0;\n");
    fprintf(f, "        if(cyc < best) { best = cyc; best_i = i; }\n");
    fprintf(f, "        if(i %% 5000 == 0) printf(\"  %%d/%%d best=%%lu\\n\", i, %d, best);\n", total);
    fprintf(f, "    }\n");
    fprintf(f, "    printf(\"Best: program %%d (%%lu cycles)\\n\", best_i, best);\n");
    fprintf(f, "    return 0;\n}\n");
    fclose(f);
    
    printf("Source file: %s\n", fn);
    printf("Compiling...\n"); fflush(stdout);
    
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "gcc -O3 -march=native -o /tmp/_census /tmp/_census.c 2>&1 | tail -3");
    int rc = system(cmd);
    
    if(rc == 0) {
        printf("Compiled OK. Binary size: ");
        fflush(stdout);
        system("ls -lh /tmp/_census | awk '{print $5}'");
        printf("\nEstimated L3 cache fit: ");
        long sz = 0;
        FILE *p = popen("ls -l /tmp/_census | awk '{print $5}'", "r");
        if(p) { fscanf(p, "%ld", &sz); pclose(p); }
        printf("%ld bytes / 24MB L3 = %.1f%%\n", sz, (float)sz/24000000*100);
        printf("Running benchmark...\n");
        system("sudo timeout 60 /tmp/_census");
    } else {
        printf("Compilation failed.\n");
        printf("Too many programs? Try reducing combinations.\n");
    }
    
    return 0;
}
