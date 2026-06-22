#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>

#define MAX_BODIES 5000
#define MAX_LINE 4096
#define MUTATION_RATE 30
#define DUP_RATE 15

static char *bodies[MAX_BODIES];
static int n_bodies = 0, n_total = 0, n_working = 0;

static void learn(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char l[MAX_LINE];
    while (fgets(l, sizeof(l), f)) {
        // Strip comments
        char *p = l; while (*p) {
            if (p[0]=='/'&&p[1]=='/') { *p=0; break; }
            if (p[0]=='/'&&p[1]=='*') { *p=' '; while(*p&&!(p[0]=='*'&&p[1]=='/')) p++; if(*p){*p=' ';p[1]=' ';} }
            p++;
        }
        // Check if it's a real statement
        char *t = l; while(*t==' '||*t=='\t') t++;
        if(!*t||*t=='\n'||*t=='#'||strstr(t,"int main")||strstr(t,"static ")||
           strstr(t,"typedef")||strstr(t,"struct")||strstr(t,"include")) continue;
        if(!strchr(t,';')&&!strchr(t,')')&&!strchr(t,'{')) continue;
        if(strstr(t,"asm volatile")||strstr(t,"//")||!strchr(t,')')) continue;
        
        // Clean it
        char s[512]; int si=0, skip=0;
        for(char *q=t;*q&&si<510;q++) {
            if(*q=='"'&&!(si>0&&s[si-1]=='\\')) skip=!skip;
            if(!skip) s[si++]=*q;
        }
        s[si]=0;
        // Remove trailing whitespace
        int sl=strlen(s); while(sl>0&&(s[sl-1]<=' ')) s[--sl]=0;
        if(sl<5) continue;
        
        // Deduplicate
        int dup=0;
        for(int i=0;i<n_bodies;i++) if(strcmp(bodies[i],s)==0) { dup=1; break; }
        if(!dup&&n_bodies<MAX_BODIES) bodies[n_bodies++]=strdup(s);
    }
    fclose(f);
}

static void mutate(char *buf, int sz) {
    // Randomly mutate a number
    for(char *p=buf;*p;p++) {
        if(*p>='0'&&*p<='9'&&rand()%100<MUTATION_RATE) {
            char old=*p;
            *p='0'+rand()%10;
            if(old!=*p) return;
        }
        // Randomly change a variable name (single letters only)
        if(*p>='a'&&*p<='z'&&rand()%100<5&&p[1]==' ') {
            *p='a'+rand()%26;
            return;
        }
    }
}

static char* generate(void) {
    static char prog[4096];
    prog[0]=0;
    int n=(rand()%3)+1, printed=0;
    
    for(int c=0;c<n;c++) {
        int idx=rand()%n_bodies;
        char buf[512]; strncpy(buf,bodies[idx],511); buf[511]=0;
        
        // Mutate
        mutate(buf,512);
        
        // Duplication
        if(rand()%100<DUP_RATE&&c>0) {
            char *dup=strdup(buf);
            strcat(prog,dup);
            strcat(prog," ");
            strcat(prog,dup);
            free(dup);
            printed=1;
        } else {
            if(c>0) strcat(prog,"\n        ");
            strcat(prog,buf);
            printed=1;
        }
    }
    if(!printed) strcat(prog,"s+=r(0x19C)&0xFF;");
    return prog;
}

int main() {
    srand(time(NULL)^time(NULL));
    printf("BREEDER V2 — extracting, mutating, recombining\n\n");
    
    DIR *d=opendir("."); if(!d) return 1;
    struct dirent *de;
    while((de=readdir(d))) {
        char *e=strrchr(de->d_name,'.');
        if(!e||strcmp(e,".c")) continue;
        if(strstr(de->d_name,"breeder")||strstr(de->d_name,"gen_")||strstr(de->d_name,"novel")) continue;
        learn(de->d_name);
    }
    closedir(d);
    printf("Learned %d statements from %d files\n\n",n_bodies,n_bodies?1:0);
    
    if(n_bodies<10) {
        printf("Too few bodies, using fallbacks\n");
        char *fb[]={"s+=r(0x19C)&0xFF;","asm volatile(\"clflush (%0)\"::\"r\"(&s):\"memory\");",
            "s+=r(0x611)&0xFF;","s+=c*7;","static int a[32]; a[c&31]++; s+=a[c&31];"};
        for(int i=0;i<5;i++) bodies[n_bodies++]=strdup(fb[i]);
    }
    
    printf("Generating programs (0-999)...\n");
    for(int gen=0;gen<1000;gen++) {
        char *prog=generate();
        char fn[64]; snprintf(fn,sizeof(fn),"gen_%04d.c",gen);
        FILE *f=fopen(fn,"w"); if(!f) continue;
        fprintf(f,"#define _GNU_SOURCE\n#include <stdio.h>\n#include <stdint.h>\n#include <fcntl.h>\n#include <unistd.h>\n\n");
        fprintf(f,"static int m; static void i(void){m=open(\"/dev/cpu/0/msr\",O_RDONLY);}\n");
        fprintf(f,"static uint64_t r(uint32_t a){uint64_t v;pread(m,&v,8,a);return v;}\n\n");
        fprintf(f,"int main(){ i(); volatile uint64_t s=0;\n for(int c=0;c<100000;c++){\n  %s\n  asm volatile(\"\": : :\"memory\");\n } printf(\"done\\n\"); return 0;}\n",prog);
        fclose(f);
        
        char cmd[256]; n_total++;
        snprintf(cmd,sizeof(cmd),"gcc -O3 -march=native -o gen_%04d gen_%04d.c 2>/dev/null",gen,gen);
        if(system(cmd)!=0) { snprintf(cmd,sizeof(cmd),"rm -f gen_%04d.c",gen); system(cmd); continue; }
        
        snprintf(cmd,sizeof(cmd),"timeout 1 ./gen_%04d > /dev/null 2>&1",gen);
        if(system(cmd)==0) {
            n_working++;
            if(n_working<=20) printf("  gen_%04d: %s\n",gen,prog);
        } else {
            snprintf(cmd,sizeof(cmd),"rm -f gen_%04d gen_%04d.c",gen,gen);
            system(cmd);
        }
        
        if(gen%100==99) printf("  ... %d/%d working\n",n_working,n_total);
    }
    
    printf("\n[Done] %d/%d programs compiled and ran\n",n_working,n_total);
    printf("Survivors: %d\n",n_working);
    system("ls -lh gen_*.c 2>/dev/null | wc -l");
    printf("files remain\n");
    return 0;
}
