/*
 * SINGULARITY BREEDER — Extracts function bodies from working programs,
 * mutates them, recombines them, generates new programs.
 * Only keeps mutations that compile and run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>

#define MAX_BODIES 2000
#define MAX_LINE 4096

static char *bodies[MAX_BODIES];
static int n_bodies = 0;
static char source_names[MAX_BODIES][256];

static void extract_bodies(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[MAX_LINE];
    char body[4096] = {0};
    int in_loop = 0, depth = 0;
    
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        
        // Find "for (int c = 0;" style loops — the singularity's main loop
        if (strstr(p, "for (") && strstr(p, "c++")) {
            in_loop = 1;
            depth = 1;
            body[0] = 0;
            continue;
        }
        
        if (in_loop) {
            // Count braces
            for (char *q = p; *q; q++) {
                if (*q == '{') depth++;
                if (*q == '}') depth--;
            }
            
            // Skip the loop body braces and the "asm volatile" barrier
            if (strstr(p, "asm volatile")) continue;
            
            // Collect the body content
            char clean[MAX_LINE];
            strncpy(clean, p, sizeof(clean)-1);
            // Remove braces
            char *c = clean;
            while (*c) {
                if (*c == '{' || *c == '}') memmove(c, c+1, strlen(c));
                else c++;
            }
            
            if (strlen(clean) > 5 && strlen(clean) < 200) {
                // Remove trailing whitespace
                int len = strlen(clean);
                while (len > 0 && (clean[len-1] == '\n' || clean[len-1] == ' ')) clean[--len] = 0;
                
                if (strlen(clean) > 5) {
                    // Check for duplicate
                    int dup = 0;
                    for (int i = 0; i < n_bodies; i++)
                        if (strcmp(bodies[i], clean) == 0) { dup = 1; break; }
                    if (!dup && n_bodies < MAX_BODIES) {
                        bodies[n_bodies] = strdup(clean);
                        strncpy(source_names[n_bodies], path, 255);
                        n_bodies++;
                    }
                }
            }
            
            if (depth <= 0) in_loop = 0;
        }
    }
    fclose(f);
}

int main() {
    srand(time(NULL));
    
    printf("BREEDER — Extracting loop bodies from %d programs\n\n", MAX_BODIES);
    
    DIR *d = opendir(".");
    if (!d) return 1;
    struct dirent *de;
    while ((de = readdir(d))) {
        char *e = strrchr(de->d_name, '.');
        if (!e || strcmp(e, ".c")) continue;
        if (strstr(de->d_name, "breeder") || strstr(de->d_name, "novel") || strstr(de->d_name, "gen_")) continue;
        extract_bodies(de->d_name);
    }
    closedir(d);
    
    printf("Extracted %d loop bodies from %d source files\n\n", n_bodies, n_bodies ? 1 : 0);
    
    if (n_bodies == 0) {
        printf("No loop bodies found. Using fallback templates.\n");
        // Define fallback bodies manually
        const char *fallbacks[] = {
            "s += r(0x19C) & 0xFF;",
            "asm volatile(\"clflush (%0)\" : : \"r\"(&s) : \"memory\");",
            "static int at[32]; at[c&31]++; s += at[c&31];",
            "uint64_t cr; asm volatile(\"mov %%cr0, %0\" : \"=r\"(cr)); s += cr;",
            "s += r(0x611) & 0xFF;",
            "s += r(0x198) & 0xFF;",
        };
        for (int i = 0; i < 6; i++) {
            bodies[i] = strdup(fallbacks[i]);
            n_bodies++;
        }
    }
    
    // Show some extracted bodies
    printf("Sample bodies:\n");
    for (int i = 0; i < (n_bodies > 10 ? 10 : n_bodies); i++)
        printf("  [%d] from %s: %s\n", i, source_names[i], bodies[i]);
    printf("\n");
    
    // Breed: recombine + mutate
    printf("Breeding new programs...\n\n");
    
    int n_success = 0;
    
    for (int gen = 0; gen < 100; gen++) {
        // Pick 1-3 bodies to combine
        int n_combo = (rand() % 3) + 1;
        char program[4096] = {0};
        
        for (int c = 0; c < n_combo; c++) {
            int idx = rand() % n_bodies;
            if (c > 0) strcat(program, "\n        ");
            strcat(program, bodies[idx]);
        }
        
        // Occasionally mutate: replace a number or variable
        if (rand() % 4 == 0) {
            char *p = program;
            while (*p) {
                if (*p >= '0' && *p <= '9') {
                    // Replace digit with another
                    *p = '0' + (rand() % 10);
                    break;
                }
                p++;
            }
        }
        
        // Generate the program file
        char fn[256];
        snprintf(fn, sizeof(fn), "breed_%03d.c", gen);
        
        FILE *f = fopen(fn, "w");
        if (!f) continue;
        
        fprintf(f, "// breed_%03d - generated by singularity_breeder\n", gen);
        fprintf(f, "#define _GNU_SOURCE\n");
        fprintf(f, "#include <stdio.h>\n");
        fprintf(f, "#include <stdint.h>\n");
        fprintf(f, "#include <fcntl.h>\n");
        fprintf(f, "#include <unistd.h>\n\n");
        fprintf(f, "static int m;\n");
        fprintf(f, "static void i(void) { m = open(\"/dev/cpu/0/msr\", O_RDONLY); }\n");
        fprintf(f, "static uint64_t r(uint32_t a) { uint64_t v; pread(m, &v, 8, a); return v; }\n\n");
        fprintf(f, "int main() {\n    i();\n    volatile uint64_t s = 0;\n");
        fprintf(f, "    for (int c = 0; c < 100000; c++) {\n");
        fprintf(f, "        %s\n", program);
        fprintf(f, "        asm volatile(\"\" : : : \"memory\");\n");
        fprintf(f, "    }\n    printf(\"done\\n\");\n    return (int)(s & 0xFF);\n}\n");
        fclose(f);
        
        // Compile
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "gcc -O3 -march=native -o breed_%03d %s 2>/dev/null", gen, fn);
        int comp = system(cmd);
        
        if (comp == 0) {
            // Test run
            snprintf(cmd, sizeof(cmd), "timeout 2 ./breed_%03d > /dev/null 2>&1", gen);
            int run = system(cmd);
            
            if (run == 0) {
                n_success++;
                printf("  gen %03d: %s\n", gen, program);
            } else {
                snprintf(cmd, sizeof(cmd), "rm -f breed_%03d breed_%03d.c", gen, gen);
                system(cmd);
            }
        } else {
            snprintf(cmd, sizeof(cmd), "rm -f breed_%03d.c", gen);
            system(cmd);
        }
    }
    
    printf("\n[Done] Generated %d programs, %d compiled and ran successfully\n", 100, n_success);
    
    // List survivors
    printf("\nSurviving programs:\n");
    system("ls -lh breed_*.c 2>/dev/null | head -20");
    
    return 0;
}
