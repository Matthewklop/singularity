#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define MAX_PROGS 512
#define MAX_LINE 4096

typedef struct { char n[256]; int l; int a[20]; int cx; } p_t;
static p_t ps[MAX_PROGS]; static int np = 0;
static char *cn[] = {"asm","msr","gpu","pmc","lbr","pt","pthr","cuda","selfm",
    "fab","stor","brain","circ","sili","mesh","pred","orb","attr","bare"};

static void an(const char *path) {
    FILE *f = fopen(path,"r"); if(!f) return;
    p_t *p = &ps[np++]; strncpy(p->n,path,255); char l[MAX_LINE];
    while(fgets(l,sizeof(l),f)) { p->l++;
        if(strstr(l,"asm"))p->a[0]=1; if(strstr(l,"msr")||strstr(l,"/dev/cpu"))p->a[1]=1;
        if(strstr(l,"resource")||strstr(l,"bar0"))p->a[2]=1; if(strstr(l,"perf")||strstr(l,"rdpmc"))p->a[3]=1;
        if(strstr(l,"lbr")||strstr(l,"0x680"))p->a[4]=1; if(strstr(l,"pt")||strstr(l,"RTIT"))p->a[5]=1;
        if(strstr(l,"pthread"))p->a[6]=1; if(strstr(l,"cuda")||strstr(l,"__global__"))p->a[7]=1;
        if(strstr(l,"fprintf")||strstr(l,"gen_"))p->a[8]=1; if(strstr(l,"fabric")||strstr(l,"cell_t"))p->a[9]=1;
        if(strstr(l,"storage"))p->a[10]=1; if(strstr(l,"neuron")||strstr(l,"synapse"))p->a[11]=1;
        if(strstr(l,"gate")||strstr(l,"transistor"))p->a[12]=1; if(strstr(l,"silicon"))p->a[13]=1;
        if(strstr(l,"mesh")||strstr(l,"slot"))p->a[14]=1; if(strstr(l,"predict")||strstr(l,"future"))p->a[15]=1;
        if(strstr(l,"orbit")||strstr(l,"period"))p->a[16]=1; if(strstr(l,"attractor"))p->a[17]=1;
        if(strstr(l,"baremetal"))p->a[18]=1;
    } fclose(f);
    for(int i=0;i<19;i++)p->cx+=p->a[i]*(i+2);
}

int main() {
    DIR *d = opendir("."); if(!d) return 1;
    struct dirent *de;
    while((de=readdir(d))) { char *e=strrchr(de->d_name,'.'); if(!e||strcmp(e,".c"))continue;
        if(strstr(de->d_name,"gen_")==de->d_name||strstr(de->d_name,"predicted"))continue; an(de->d_name); }
    closedir(d);

    int caps[20]={0};
    for(int i=0;i<np;i++) for(int j=0;j<19;j++) caps[j]+=ps[i].a[j];

    int l=0,lv=999,m=0,mv=0;
    for(int i=0;i<19;i++) {
        printf("  %-8s %d/%d (%.0f%%)\n",cn[i],caps[i],np,(float)caps[i]/np*100);
        if(caps[i]<lv&&caps[i]>0){lv=caps[i];l=i;} if(caps[i]>mv){mv=caps[i];m=i;}
    }

    printf("\n → fuse %s + %s\n\n",cn[m],cn[l]);

    char fn[256]; snprintf(fn,sizeof(fn),"predicted_%s_%s.c",cn[m],cn[l]);
    FILE *f=fopen(fn,"w"); if(!f) return 1;

    fprintf(f,"// AUTO-GENERATED: fuse %s + %s\n",cn[m],cn[l]);
    fprintf(f,"#define _GNU_SOURCE\n#include <stdio.h>\n#include <stdint.h>\n#include <fcntl.h>\n#include <unistd.h>\n\n");
    fprintf(f,"static int msr_fd;\nstatic void init(void){msr_fd=open(\"/dev/cpu/0/msr\",O_RDONLY);}\n");
    fprintf(f,"static uint64_t rd(uint32_t a){uint64_t v;pread(msr_fd,&v,8,a);return v;}\n\n");
    fprintf(f,"int main(){\n init();\n volatile uint64_t s=0;\n");
    fprintf(f," for(int c=0;c<100000;c++){\n");
    fprintf(f,"  s+=rd(0x19C)&0xFF;\n");
    if(l==2) fprintf(f,"  int g=open(\"/sys/bus/pci/devices/0000:01:00.0/resource0\",O_RDWR);\n");
    if(l==3||l==4||l==5) fprintf(f,"  asm volatile(\"clflush (%%0)\"::\"r\"(&s):\"memory\");\n");
    if(l==9||l==10) fprintf(f,"  static uint64_t fab[65536]; fab[c&65535]++;\n");
    fprintf(f,"  asm volatile(\"\": : :\"memory\");\n }\n");
    fprintf(f," printf(\"[%%s] done\\n\",\"%s_%s\");\n return 0;\n}\n",cn[m],cn[l]);
    fclose(f);

    printf("→ %s ",fn); fflush(stdout);
    char cmd[256]; snprintf(cmd,sizeof(cmd),"gcc -O3 -o predicted_%s_%s %s 2>/dev/null",cn[m],cn[l],fn);
    if(system(cmd)==0){ printf("✅\n");
        snprintf(cmd,sizeof(cmd),"./predicted_%s_%s",cn[m],cn[l]);
        system(cmd);
    } else printf("❌\n");
    return 0;
}
