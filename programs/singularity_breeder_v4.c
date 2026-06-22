#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>

#define MAX_BODIES 20000
#define MAX_LINE 4096

static char *bodies[MAX_BODIES];
static int n_bodies = 0;

/* Core seed patterns that always work */
static const char *seeds[] = {
    "s += r(0x19C) & 0xFF;",
    "s += r(0x198) & 0xFF;",
    "s += r(0x611) & 0xFF;",
    "s += r(0xE8) & 0xFF;",
    "s += c * 7;", "s += c * 13;", "s += c * 3;",
    "s += c & 0xFF;", "s += (c >> 8) & 0xFF;",
    "s += (c * 7 + 13) & 0xFF;", "s += c % 32;", "s += (c / 32) & 0xFF;",
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

static void learn(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char l[MAX_LINE];
    while (fgets(l, sizeof(l), f)) {
        char *t = l; while (*t==' '||*t=='\t') t++;
        if (!*t||*t=='\n'||*t=='#'||strstr(t,"#include")||
            strstr(t,"int main")||strstr(t,"static ")||
            strstr(t,"typedef")||strstr(t,"struct")) continue;
        if (!strchr(t,';')&&!strchr(t,'}')) continue;
        
        char s[512]; int si=0;
        for(char *q=t;*q&&si<510;q++){
            if(q[0]=='/'&&q[1]=='/') break;
            if(q[0]=='/'&&q[1]=='*'){while(*q&&!(q[0]=='*'&&q[1]=='/'))q++;if(*q)q++;continue;}
            s[si++]=*q;
        }
        s[si]=0;
        int sl=strlen(s); while(sl>0&&s[sl-1]<=' ')s[--sl]=0;
        if(sl<4) continue;
        if(s[0]=='{'&&s[sl-1]=='}'){/* compound stmt */} 
        else if(s[sl-1]!=';') continue;
        
        int dup=0;
        for(int i=0;i<n_bodies;i++) if(strcmp(bodies[i],s)==0){dup=1;break;}
        if(!dup&&n_bodies<MAX_BODIES) bodies[n_bodies++]=strdup(s);
    }
    fclose(f);
}

static void gen_prog(int g) {
    int n = (rand()%4)+1;
    char prog[4096]={0};
    
    for(int c=0;c<n;c++){
        const char *src;
        if(rand()%10<7||n_bodies==0) src=seeds[rand()%n_seeds];
        else src=bodies[rand()%n_bodies];
        
        char buf[1024]; strncpy(buf,src,1023); buf[1023]=0;
        
        // Mutate numbers
        for(char*p=buf;*p;p++)
            if(*p>='0'&&*p<='9'&&rand()%100<20){
                *p='0'+rand()%10;
                break;
            }
        
        if(c>0) strcat(prog,"\n        ");
        strcat(prog,buf);
        
        // Occasionally duplicate
        if(rand()%15==0){ strcat(prog,"\n        "); strcat(prog,buf); }
    }
    
    char fn[64]; snprintf(fn,sizeof(fn),"b_%04d.c",g);
    FILE*f=fopen(fn,"w"); if(!f) return;
    fprintf(f,"#define _GNU_SOURCE\n#include <stdio.h>\n#include <stdint.h>\n#include <fcntl.h>\n#include <unistd.h>\n\n");
    fprintf(f,"static int m; static void i(void){m=open(\"/dev/cpu/0/msr\",O_RDONLY);}\n");
    fprintf(f,"static uint64_t r(uint32_t a){uint64_t v;pread(m,&v,8,a);return v;}\n\n");
    fprintf(f,"int main(){ i(); volatile uint64_t s=0;\n for(int c=0;c<100000;c++){\n  %s\n  asm volatile(\"\": : :\"memory\");\n } printf(\"done\\n\"); return (int)(s&0xFF);}\n",prog);
    fclose(f);
    
    char cmd[512];
    snprintf(cmd,sizeof(cmd),"gcc -O3 -march=native -o b_%04d b_%04d.c 2>/dev/null",g,g);
    if(system(cmd)!=0){ snprintf(cmd,sizeof(cmd),"rm -f b_%04d.c",g); system(cmd); return; }
    
    snprintf(cmd,sizeof(cmd),"timeout 2 ./b_%04d > /dev/null 2>&1",g);
    if(system(cmd)==0){
        static int shown=0;
        if(shown<30){ printf("  [%04d] %s\n",g,prog); shown++; }
    } else {
        snprintf(cmd,sizeof(cmd),"rm -f b_%04d b_%04d.c",g,g);
        system(cmd);
    }
}

int main(){
    srand(time(NULL));
    printf("BREEDER V4 — 1000 generations\n\n");
    
    DIR*d=opendir("."); if(!d) return 1;
    struct dirent *de;
    while((de=readdir(d))){
        char*e=strrchr(de->d_name,'.');
        if(!e||strcmp(e,".c")) continue;
        if(strstr(de->d_name,"breeder")||strstr(de->d_name,"b_")) continue;
        learn(de->d_name);
    }
    closedir(d);
    printf("Seeds: %d, Learned: %d\n\n",n_seeds,n_bodies);
    
    for(int g=0;g<1000;g++){ gen_prog(g); if(g%100==99) printf("  ...%d/1000\n",g+1); }
    
    printf("\nSurvivors:\n");
    system("ls b_*.c 2>/dev/null | wc -l");
    return 0;
}
