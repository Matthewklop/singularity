#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

#define MAX_POP 500
#define MAX_LINE 4096
#define GEN_MAX 50
#define POP_SIZE 100

static char *seeds[] = {
    "s += r(0x19C) & 0xFF;",
    "s += r(0x198) & 0xFF;", "s += r(0x611) & 0xFF;", "s += r(0xE8) & 0xFF;",
    "s += c * 7;", "s += c * 13;", "s += c & 0xFF;",
    "s += (c >> 8) & 0xFF;", "s += (c * 7 + 13) & 0xFF;", "s += c % 32;",
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

typedef struct { char code[4096]; double fitness; int gen; } organism_t;
static organism_t pop[MAX_POP];
static int n_pop = 0, gen = 0;

static double bench(const char *bin) {
    char cmd[512]; snprintf(cmd,sizeof(cmd),"sudo perf stat -e cycles -x ',' ./%s 2>&1",bin);
    FILE *f = popen(cmd,"r"); if(!f) return 1e30;
    char l[256]; double c=1e30;
    while(fgets(l,sizeof(l),f)) { double v; char n[64]; if(sscanf(l,"%lf,,%[^,]",&v,n)>=2&&strstr(n,"cycles")) c=v; }
    pclose(f); return c;
}

static int try_compile(const char *name, const char *body) {
    char fn[256]; snprintf(fn,sizeof(fn),"%s.c",name);
    FILE *f = fopen(fn,"w"); if(!f) return 0;
    fprintf(f,"#define _GNU_SOURCE\n#include <stdio.h>\n#include <stdint.h>\n#include <fcntl.h>\n#include <unistd.h>\n\n");
    fprintf(f,"static int m; static void i(void){m=open(\"/dev/cpu/0/msr\",O_RDONLY);}\n");
    fprintf(f,"static uint64_t r(uint32_t a){uint64_t v;pread(m,&v,8,a);return v;}\n\n");
    fprintf(f,"int main(){ i(); volatile uint64_t s=0;\n for(int c=0;c<100000;c++){\n  %s\n  asm volatile(\"\": : :\"memory\");\n } printf(\"done\\n\"); return (int)(s&0xFF);}\n",body);
    fclose(f);
    char cmd[512]; snprintf(cmd,sizeof(cmd),"gcc -O3 -march=native -o %s %s 2>/dev/null",name,fn);
    return system(cmd) == 0;
}

static char* generate(int parent1, int parent2) {
    static char buf[4096];
    buf[0] = 0;
    
    // Crossover: take some lines from parent1, some from parent2
    char *lines1[64], *lines2[64];
    int n1 = 0, n2 = 0;
    char *p = pop[parent1].code;
    while(*p && n1 < 64) { lines1[n1++] = p; p = strchr(p, '\n'); if(p) { *p = 0; p++; } }
    p = pop[parent2].code;
    while(*p && n2 < 64) { lines2[n2++] = p; p = strchr(p, '\n'); if(p) { *p = 0; p++; } }
    
    for(int i = 0; i < 4; i++) {
        if(i > 0) strcat(buf, "\n        ");
        int take_from = (rand() % 2) ? 1 : 2;
        int idx;
        if(take_from == 1 && n1 > 0) { idx = rand() % n1; strcat(buf, lines1[idx]); }
        else if(n2 > 0) { idx = rand() % n2; strcat(buf, lines2[idx]); }
        else strcat(buf, seeds[rand() % n_seeds]);
    }
    
    // Mutate
    for(char *q = buf; *q; q++)
        if(*q >= '0' && *q <= '9' && rand() % 20 == 0) { *q = '0' + rand() % 10; break; }
    
    return buf;
}

int main() {
    srand(time(NULL));
    printf("EVOLUTION — breeding for performance\n\n");
    
    system("sudo modprobe msr 2>/dev/null");
    
    // Generation 0: seed with random programs
    printf("Gen 0: creating %d random programs...\n", POP_SIZE);
    for(int i = 0; i < POP_SIZE; i++) {
        char body[4096] = {0};
        int n = (rand() % 3) + 1;
        for(int j = 0; j < n; j++) {
            if(j > 0) strcat(body, "\n        ");
            strcat(body, seeds[rand() % n_seeds]);
        }
        char name[64]; snprintf(name, sizeof(name), "evo_%03d", i);
        if(try_compile(name, body)) {
            pop[n_pop].fitness = bench(name);
            strncpy(pop[n_pop].code, body, 4095);
            pop[n_pop].gen = 0;
            n_pop++;
        }
        if(i % 20 == 19) printf("  %d/%d\n", i+1, POP_SIZE);
    }
    printf("  %d survived\n\n", n_pop);
    
    // Sort by fitness
    for(int i = 0; i < n_pop; i++)
        for(int j = i+1; j < n_pop; j++)
            if(pop[j].fitness < pop[i].fitness) {
                organism_t t = pop[i]; pop[i] = pop[j]; pop[j] = t;
            }
    
    printf("Best gen 0: %.0f cycles\n", pop[0].fitness);
    
    // Evolve
    for(gen = 1; gen < GEN_MAX; gen++) {
        int n_new = 0;
        char buf[64];
        
        for(int i = 0; i < POP_SIZE/2; i++) {
            int p1 = rand() % (n_pop/3 + 1);  // Weight toward best
            int p2 = rand() % (n_pop/2 + 1);
            if(p2 == p1) p2 = (p2 + 1) % n_pop;
            
            char *body = generate(p1, p2);
            snprintf(buf, sizeof(buf), "evo_g%02d_%03d", gen, i);
            if(try_compile(buf, body)) {
                double fit = bench(buf);
                if(fit < pop[n_pop-1].fitness) {  // Replace worst
                    pop[n_pop-1].fitness = fit;
                    strncpy(pop[n_pop-1].code, body, 4095);
                    pop[n_pop-1].gen = gen;
                    // Re-sort
                    for(int k = n_pop-1; k > 0 && pop[k].fitness < pop[k-1].fitness; k--) {
                        organism_t t = pop[k]; pop[k] = pop[k-1]; pop[k-1] = t;
                    }
                }
            }
            snprintf(buf, sizeof(buf), "rm -f evo_g%02d_%03d evo_g%02d_%03d.c", gen, i, gen, i);
            system(buf);
        }
        
        printf("Gen %2d: best=%.0f pop=%d\n", gen, pop[0].fitness, n_pop);
        
        if(pop[0].fitness < 500000) {
            printf("\nConverged at gen %d\n", gen);
            break;
        }
    }
    
    printf("\n=== FINAL BEST ===\n");
    printf("Fitness: %.0f cycles\n", pop[0].fitness);
    printf("Code:\n%s\n", pop[0].code);
    
    // Cleanup
    system("rm -f evo_* evo_*.c 2>/dev/null");
    return 0;
}
