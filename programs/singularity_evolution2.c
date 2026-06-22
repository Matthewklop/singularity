#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define MAX_POP 500
#define MAX_LINE 4096

static char *seeds[] = {
    "s += r(0x19C) & 0xFF;", "s += r(0x198) & 0xFF;",
    "s += r(0x611) & 0xFF;", "s += r(0xE8) & 0xFF;",
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

typedef struct { char code[4096]; double fitness; } org_t;
static org_t pop[MAX_POP];
static int n_pop = 0;

static double bench(const char *bin) {
    char cmd[512]; snprintf(cmd,sizeof(cmd),"sudo perf stat -e cycles -x ',' ./%s 2>&1",bin);
    FILE *f = popen(cmd,"r"); if(!f) return 1e30;
    char l[256]; double c=1e30;
    while(fgets(l,sizeof(l),f)) { double v; char n[64]; if(sscanf(l,"%lf,,%[^,]",&v,n)>=2&&strstr(n,"cycles")) c=v; }
    pclose(f); return c;
}

static int mkprog(const char *name, const char *body) {
    char fn[512]; snprintf(fn,sizeof(fn),"%s.c",name);
    FILE *f = fopen(fn,"w"); if(!f) return 0;
    fprintf(f,"#define _GNU_SOURCE\n#include <stdio.h>\n#include <stdint.h>\n#include <fcntl.h>\n#include <unistd.h>\n\n");
    fprintf(f,"static int m; static void i(void){m=open(\"/dev/cpu/0/msr\",O_RDONLY);}\n");
    fprintf(f,"static uint64_t r(uint32_t a){uint64_t v;pread(m,&v,8,a);return v;}\n\n");
    fprintf(f,"int main(){ i(); volatile uint64_t s=0;\n for(int c=0;c<100000;c++){\n  %s\n  asm volatile(\"\": : :\"memory\");\n } printf(\"done\\n\"); return (int)(s&0xFF);}\n",body);
    fclose(f);
    char cmd[512]; snprintf(cmd,sizeof(cmd),"gcc -O3 -march=native -o %s %s 2>/dev/null",name,fn);
    return system(cmd) == 0;
}

static void mutate(char *buf) {
    for(char *p = buf; *p; p++)
        if(*p >= '0' && *p <= '9' && rand() % 100 < 15) {
            *p = '0' + rand() % 10;
            return;
        }
    // Sometimes swap two adjacent tokens
    for(char *p = buf; *p && p[1]; p++) {
        if((*p >= 'a' && *p <= 'z') && (p[1] >= 'a' && p[1] <= 'z') && rand() % 100 < 5) {
            char t = *p; *p = p[1]; p[1] = t;
            return;
        }
    }
}

int main() {
    srand(time(NULL));
    printf("EVOLUTION2\n\n");
    system("sudo modprobe msr 2>/dev/null");
    
    // Gen 0
    printf("Gen 0: ");
    for(int i = 0; i < 100 && n_pop < MAX_POP; i++) {
        char body[4096] = {0};
        int n = (rand() % 4) + 1;
        for(int j = 0; j < n; j++) {
            if(j > 0) strcat(body, "\n        ");
            strcat(body, seeds[rand() % n_seeds]);
        }
        char name[64]; snprintf(name,sizeof(name),"e%03d",i);
        if(mkprog(name,body)) {
            double fit = bench(name);
            if(fit < 1e29) { pop[n_pop].fitness = fit; strncpy(pop[n_pop].code,body,4095); n_pop++; }
        }
        snprintf(name,sizeof(name),"rm -f e%03d e%03d.c",i,i);
        system(name);
    }
    printf("%d survivors\n", n_pop);
    
    // Sort
    for(int i = 0; i < n_pop; i++)
        for(int j = i+1; j < n_pop; j++)
            if(pop[j].fitness < pop[i].fitness) { org_t t = pop[i]; pop[i] = pop[j]; pop[j] = t; }
    
    printf("Best: %.0f cycles\n\n", pop[0].fitness);
    
    // Evolve
    for(int gen = 1; gen < 30; gen++) {
        int replaced = 0;
        for(int i = 0; i < 50; i++) {
            // Pick two parents from top 50%
            int p1 = rand() % (n_pop/2); if(p1 < 0) p1 = 0;
            int p2 = rand() % (n_pop/2);
            
            // Crossover: split each parent's code into lines, recombine
            char lines1[10][256]; int n1 = 0;
            char lines2[10][256]; int n2 = 0;
            char tmp[4096]; strncpy(tmp,pop[p1].code,4095);
            char *tok = strtok(tmp,"\n");
            while(tok && n1 < 10) { strncpy(lines1[n1++],tok,255); tok = strtok(NULL,"\n"); }
            strncpy(tmp,pop[p2].code,4095);
            tok = strtok(tmp,"\n");
            while(tok && n2 < 10) { strncpy(lines2[n2++],tok,255); tok = strtok(NULL,"\n"); }
            
            char body[4096] = {0};
            for(int j = 0; j < 4; j++) {
                if(j > 0) strcat(body, "\n        ");
                if(rand() % 2 && n1 > 0) strcat(body, lines1[rand() % n1]);
                else if(n2 > 0) strcat(body, lines2[rand() % n2]);
                else strcat(body, seeds[rand() % n_seeds]);
            }
            mutate(body);
            
            char name[64]; snprintf(name,sizeof(name),"g%02d_%03d",gen,i);
            if(mkprog(name,body)) {
                double fit = bench(name);
                if(fit < pop[n_pop-1].fitness) {
                    pop[n_pop-1].fitness = fit;
                    strncpy(pop[n_pop-1].code,body,4095);
                    // Re-sort
                    for(int k = n_pop-1; k > 0 && pop[k].fitness < pop[k-1].fitness; k--) {
                        org_t t = pop[k]; pop[k] = pop[k-1]; pop[k-1] = t;
                    }
                    replaced++;
                }
            }
            snprintf(name,sizeof(name),"rm -f g%02d_%03d g%02d_%03d.c",gen,i,gen,i);
            system(name);
        }
        printf("Gen %2d: best=%.0f replaced=%d\n", gen, pop[0].fitness, replaced);
        
        if(pop[0].fitness < 500000) { printf("\nConverged\n"); break; }
    }
    
    printf("\n=== BEST ===\n%.0f cycles\n%s\n", pop[0].fitness, pop[0].code);
    return 0;
}
