/* ============================================================================
 * oracle_self_optimize.c — Watches Minecraft forever. Optimizes live.
 *
 * Build: gcc -O3 -o oracle_self_optimize oracle_self_optimize.c -lm
 * Run:   nohup stdbuf -oL ./oracle_self_optimize &
 * ============================================================================
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include <sys/resource.h>
#include <sched.h>

#define P "/oracle_self_optimize"
#define BUF 512

typedef struct { uint64_t magic,cycle,adj,sessions; int bgc,bpri,baff; double bg; } St;

static pid_t find_mc(void) {
    DIR*d=opendir("/proc");if(!d)return 0;
    struct dirent*de;
    while((de=readdir(d))){
        if(de->d_name[0]<'0'||de->d_name[0]>'9')continue;
        pid_t p=atoi(de->d_name);
        char pt[BUF],b[BUF];snprintf(pt,BUF,"/proc/%d/comm",p);
        int f=open(pt,O_RDONLY);if(f<0)continue;
        int n=read(f,b,BUF-1);close(f);if(n<=0)continue;
        b[n]=0;
        if(strstr(b,"java")){closedir(d);return p;}
    }
    closedir(d);return 0;
}

int main(void){
    int fd=shm_open(P,O_CREAT|O_RDWR,0644);
    if(fd<0)return 1;ftruncate(fd,sizeof(St));
    St*s=mmap(0,sizeof(St),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);
    if(s==MAP_FAILED)return 1;
    if(s->magic!=0x4F5054){memset(s,0,sizeof(St));s->magic=0x4F5054;s->bpri=0;s->baff=4;s->bgc=2;s->bg=999999;}

    printf("ORACLE SELF-OPTIMIZE DAEMON\n");
    printf("Best: pri=%d aff=%d gc=%d sessions=%lu\n\n",s->bpri,s->baff,s->bgc,(unsigned long)s->sessions);

    while(1){
        pid_t mc=find_mc();
        if(!mc){sleep(2);continue;}

        s->sessions++;
        printf("SESSION %lu: PID=%d\n",(unsigned long)s->sessions,mc);
        printf("  Applying pri=%d aff=%d\n",s->bpri,s->baff);
        setpriority(PRIO_PROCESS,mc,s->bpri);
        cpu_set_t cm;CPU_ZERO(&cm);for(int i=0;i<s->baff;i++)CPU_SET(i,&cm);
        sched_setaffinity(mc,sizeof(cm),&cm);

        double lg=0;int sa=0,tg=s->bgc,pri=s->bpri,aff=s->baff;
        while(1){
            char pt[BUF];snprintf(pt,BUF,"/proc/%d",mc);
            if(access(pt,F_OK)!=0){printf("  Closed.\n\n");break;}

            double heap=0,gc=0;
            snprintf(pt,BUF,"/proc/%d/status",mc);
            int f=open(pt,O_RDONLY);
            if(f>=0){char b[BUF];int n=read(f,b,BUF-1);close(f);
                if(n>0){b[n]=0;char*v=strstr(b,"VmRSS:");if(v)heap=atof(v+7)/1024.0;}}

            snprintf(pt,BUF,"/proc/%d/stat",mc);
            f=open(pt,O_RDONLY);
            if(f>=0){char b[BUF];int n=read(f,b,BUF-1);close(f);
                if(n>0){char*p=b;int fl=0;while(*p&&fl<13){if(*p==' ')fl++;p++;}
                    if(fl>=13){double ut=atof(p);while(*p&&*p!=' ')p++;double st=atof(p);
                    gc=(ut+st)/sysconf(_SC_CLK_TCK)*1000.0;}}}
            if(heap==0&&gc==0){sleep(5);continue;}

            double gd=gc-lg;if(gd<0)gd=0;lg=gc;
            double fps=gd<5?60:(gd<20?30:(gd<50?15:5));
            if(gd>0&&gd<s->bg)s->bg=gd;

            printf("  C%lu H%.0fMB GC%.0fms FPS%.0f adj=%d\n",(unsigned long)s->cycle,heap,gd,fps,sa);

            if(gd>20&&fps<30){
                s->adj++;sa++;
                if(tg<4){tg++;snprintf(pt,BUF,"jcmd %d VM.set_flag ConcGCThreads %d 2>/dev/null",mc,tg);system(pt);printf("    GC=%d\n",tg);}
                else if(aff<8){aff+=2;cpu_set_t c2;CPU_ZERO(&c2);for(int i=0;i<aff;i++)CPU_SET(i,&c2);sched_setaffinity(mc,sizeof(c2),&c2);printf("    AFF=%d\n",aff);}
                else if(pri>-20){pri-=5;setpriority(PRIO_PROCESS,mc,pri);printf("    PRI=%d\n",pri);}
            }
            s->bpri=pri;s->baff=aff;s->bgc=tg;
            s->cycle++;sleep(5);
        }
    }
    munmap(s,sizeof(St));return 0;
}
