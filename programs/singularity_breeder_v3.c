#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>

#define MAX_BODIES 10000
#define MAX_LINE 4096

static char *bodies[MAX_BODIES];
static int n_bodies = 0;

/* Known-good statement patterns that compile */
static const char *seed_patterns[] = {
    "s += r(0x19C) & 0xFF;",
    "s += r(0x198) & 0xFF;",
    "s += r(0x611) & 0xFF;",
    "s += r(0xE8) & 0xFF;",
    "s += c * 7;",
    "s += c * 13;",
    "s += c * 3;",
    "s += c & 0xFF;",
    "s += (c >> 8) & 0xFF;",
    "s += (c * 7 + 13) & 0xFF;",
    "s += c % 32;",
    "s += (c / 32) & 0xFF;",
    "asm volatile(\"clflush (%0)\" : : \"r\"(&s) : \"memory\");",
    "asm volatile(\"mfence\" : : : \"memory\");",
    "asm volatile(\"pause\" : : : \"memory\");",
    "{ uint64_t cr; asm volatile(\"mov %%cr0, %0\" : \"=r\"(cr)); s += cr; }",
    "{ uint64_t cr; asm volatile(\"mov %%cr2, %0\" : \"=r\"(cr)); s += cr; }",
    "{ uint32_t lo; asm volatile(\"rdpmc\" : \"=a\"(lo) : \"c\"(0) : \"edx\"); s += lo; }",
    "static int at[32]; at[c & 31]++; s += at[c & 31];",
    "static uint64_t fab[256]; fab[c & 255]++; s += fab[c & 255];",
    "s += (uint64_t)(c * 7 + c * 13 + c * 3);",
    "s ^= (c * 0x9E3779B97F4A7C15ULL) >> 32;",
    "s ^= s >> 12; s ^= s << 25; s ^= s >> 27;",
    "static uint64_t x = 0x9E3779B97F4A7C15ULL; x ^= x >> 12; x ^= x << 25; x ^= x >> 27; s += x;",
};

static int n_seeds = sizeof(seed_patterns) / sizeof(seed_patterns[0]);

static void learn(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char l[MAX_LINE];
    while (fgets(l, sizeof(l), f)) {
        char *t = l; while (*t == ' ' || *t == '\t') t++;
        if (!*t || *t == '\n' || *t == '#' || strstr(t, "int main") || 
            strstr(t, "static ") || strstr(t, "typedef") || strstr(t, "struct") ||
            strstr(t, "include")) continue;
        if (!strchr(t, ';') && !strchr(t, '{')) continue;
        if (strstr(t, "asm volatile")) continue; // we have these as seeds
        
        // Clean: remove comments
        char s[512]; int si = 0;
        for (char *q = t; *q && si < 510; q++) {
            if (q[0] == '/' && q[1] == '/') break;
            if (q[0] == '/' && q[1] == '*') { while (*q && !(q[0] == '*' && q[1] == '/')) q++; if (*q) q++; continue; }
            s[si++] = *q;
        }
        s[si] = 0;
        int sl = strlen(s);
        while (sl > 0 && s[sl-1] <= ' ') s[--sl] = 0;
        if (sl < 8) continue;
        
        // Must contain a semicolon or brace at the end
        if (s[sl-1] != ';' && s[sl-1] != '}') continue;
        
        int dup = 0;
        for (int i = 0; i < n_bodies; i++) if (strcmp(bodies[i], s) == 0) { dup = 1; break; }
        if (!dup && n_bodies < MAX_BODIES) bodies[n_bodies++] = strdup(s);
    }
    fclose(f);
}

static char* generate(void) {
    static char prog[4096];
    prog[0] = 0;
    int n = (rand() % 4) + 1;
    
    for (int c = 0; c < n; c++) {
        // Pick from seeds or learned bodies
        const char *src;
        int pick_seed = (n_bodies == 0 || rand() % 3 > 0);
        if (pick_seed) src = seed_patterns[rand() % n_seeds];
        else src = bodies[rand() % n_bodies];
        
        // Duplicate with mutation sometimes
        if (rand() % 10 == 0 && c > 0) {
            strcat(prog, src);
            strcat(prog, " ");
            // Mutate: change number
            char buf[512]; strncpy(buf, src, 511); buf[511] = 0;
            for (char *p = buf; *p; p++)
                if (*p >= '0' && *p <= '9' && rand() % 5 == 0) { *p = '0' + rand() % 10; break; }
            strcat(prog, buf);
        } else {
            if (c > 0) strcat(prog, "\n        ");
            strcat(prog, src);
        }
    }
    return prog;
}

int main() {
    srand(time(NULL));
    printf("BREEDER V3 — seeds + learning + mutation\n\n");
    
    // Learn from existing programs
    DIR *d = opendir("."); if (!d) return 1;
    struct dirent *de;
    while ((de = readdir(d))) {
        char *e = strrchr(de->d_name, '.');
        if (!e || strcmp(e, ".c")) continue;
        if (strstr(de->d_name, "breeder") || strstr(de->d_name, "gen_")) continue;
        learn(de->d_name);
    }
    closedir(d);
    
    printf("Seed patterns: %d\n", n_seeds);
    printf("Learned bodies: %d\n", n_bodies);
    
    int n_working = 0;
    for (int gen = 0; gen < 500; gen++) {
        char *prog = generate();
        char fn[64]; snprintf(fn, sizeof(fn), "gen_%04d.c", gen);
        FILE *f = fopen(fn, "w"); if (!f) continue;
        fprintf(f, "#define _GNU_SOURCE\n#include <stdio.h>\n#include <stdint.h>\n#include <fcntl.h>\n#include <unistd.h>\n\n");
        fprintf(f, "static int m; static void i(void){m=open(\"/dev/cpu/0/msr\",O_RDONLY);}\n");
        fprintf(f, "static uint64_t r(uint32_t a){uint64_t v;pread(m,&v,8,a);return v;}\n\n");
        fprintf(f, "int main(){ i(); volatile uint64_t s=0;\n for(int c=0;c<100000;c++){\n  %s\n  asm volatile(\"\": : :\"memory\");\n } printf(\"done\\n\"); return (int)(s&0xFF);}\n", prog);
        fclose(f);
        
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "gcc -O3 -march=native -o gen_%04d gen_%04d.c 2>/dev/null", gen, gen);
        if (system(cmd) != 0) { snprintf(cmd, sizeof(cmd), "rm -f gen_%04d.c", gen); system(cmd); continue; }
        
        snprintf(cmd, sizeof(cmd), "timeout 1 ./gen_%04d > /dev/null 2>&1", gen);
        if (system(cmd) == 0) {
            n_working++;
            if (n_working <= 20) printf("  [%04d] %s\n", gen, prog);
        } else {
            snprintf(cmd, sizeof(cmd), "rm -f gen_%04d gen_%04d.c", gen, gen);
            system(cmd);
        }
        
        if (gen > 0 && gen % 100 == 0) printf("  ... %d/%d so far\n", n_working, gen);
    }
    
    printf("\n[Done] %d/500 working programs\n", n_working);
    return 0;
}
