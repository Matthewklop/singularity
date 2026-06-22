#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

static double bfit = 1e30;
static int gen = 0;
static char bname[256] = {0};

static double bench(const char *bin) {
    char cmd[512]; snprintf(cmd,sizeof(cmd),"sudo perf stat -e cycles -x ',' ./%s 2>&1",bin);
    FILE *f = popen(cmd,"r"); if(!f) return 1e30;
    char l[256]; double c=1e30;
    while(fgets(l,sizeof(l),f)) { double v; char n[64]; if(sscanf(l,"%lf,,%[^,]",&v,n)>=2&&strstr(n,"cycles")) c=v; }
    pclose(f); return c;
}

static void genprog(const char *name, const char *body) {
    char fn[256]; snprintf(fn,sizeof(fn),"e_%s_%d.c",name,gen);
    FILE *f = fopen(fn,"w"); if(!f) return;
    fprintf(f,"#define _GNU_SOURCE\n#include <stdio.h>\n#include <stdint.h>\n#include <fcntl.h>\n#include <unistd.h>\n\n");
    fprintf(f,"static int m; static void i(void){m=open(\"/dev/cpu/0/msr\",O_RDONLY);}\n");
    fprintf(f,"static uint64_t r(uint32_t a){uint64_t v;pread(m,&v,8,a);return v;}\n\n");
    fprintf(f,"int main(){ i(); volatile uint64_t s=0;\n for(int c=0;c<100000;c++){\n  %s\n  asm volatile(\"\": : :\"memory\");\n } printf(\"done\\n\"); return 0;}\n",body);
    fclose(f);
    char cmd[256]; snprintf(cmd,sizeof(cmd),"gcc -O3 -march=native -o e_%s_%d %s 2>/dev/null",name,gen,fn);
    system(cmd);
}

int main() {
    srand(time(NULL));
    double fit;
    int i,j;
    
    printf("SINGULARITY FINAL EVOLUTION\n\n");
    
    // Phase 1: Discover best single capability
    printf("Phase 1: Single capabilities\n");
    char *caps[] = {
        "s+=r(0x19C)&0xFF;",                           // msr
        "asm volatile(\"clflush (%%0)\"::\"r\"(&s):\"memory\");", // asm
        "uint64_t cr; asm volatile(\"mov %%cr0,%%0\":\"=r\"(cr)); s+=cr;", // bare
        "s+=(c*7+13)&0xFF;",                            // predict
        "static int a[32]; a[c&31]++; s+=a[c&31];",     // attractor
        "s+=(c%%32);",                                  // orbit
        "static uint64_t f[256]; f[c&255]++; s+=f[c&255];", // fabric
        "s+=r(0x611)&0xFF;",                            // mesh
    };
    int nc = sizeof(caps)/sizeof(caps[0]);
    int bi = 0;
    for(i=0;i<nc;i++) {
        genprog(caps[i],caps[i]); char bn[64]; snprintf(bn,sizeof(bn),"e_%d",gen);
        // fix name
        char fn[256]; snprintf(fn,sizeof(fn),"e_%s_%d",caps[i],gen);
        if(access(fn,X_OK)!=0) continue;
        fit = bench(fn);
        printf("  %d: %.0f\n",i,fit);
        if(fit<bfit){bfit=fit;bi=i;strcpy(bname,caps[i]);}
    }
    printf("  Best: cap%d %.0f\n\n",bi,bfit);
    
    // Phase 2: Fuse best with others
    printf("Phase 2: Fusions\n");
    for(i=0;i<50;i++) {
        int j = rand() % nc;
        if(j==bi) j=(j+1)%nc;
        char body[512]; snprintf(body,sizeof(body),"%s\n        %s",caps[bi],caps[j]);
        genprog(body,body);
        char fn[256]; snprintf(fn,sizeof(fn),"e_%s_%d",body,gen);
        if(access(fn,X_OK)!=0) continue;
        fit = bench(fn);
        if(fit<bfit){bfit=fit;printf("  gen %d: fusion cap%d+cap%d %.0f ← NEW BEST\n",gen,bi,j,fit);}
        else printf("  gen %d: %.0f\n",gen,fit);
        gen++;
    }
    
    printf("\n[Done] Best: %.0f cycles\n",bfit);
    return 0;
}
