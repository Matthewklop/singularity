/* ============================================================================
 * oracle_mesh_singularity2.c — Reads from unified mesh state
 *
 * Reads all 10 oracle tools from oracle_mesh_state shared memory.
 * Feeds their values into 8 attractors.
 * The singularity converges the entire mesh.
 *
 * Build: gcc -O3 -o oracle_mesh_singularity2 oracle_mesh_singularity2.c -lm -lrt
 * Run:   ./oracle_mesh_singularity2 [silent]
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
#include <math.h>

#define P "/oracle_mesh_singularity2"
#define STATE_PATH "/oracle_mesh_state"
#define N_ATTRACTORS 8
#define N_RULES 2048
#define N_HISTORY 32

/* ─── Mesh state structure (must match oracle_mesh_state.c) ─── */
#define MAX_TOOLS 32
#define NAME_LEN 24
typedef struct {
    uint64_t magic,ts,cycle; double v1,v2,v3,res; char name[NAME_LEN];
    uint8_t pad[640-32-24];
} __attribute__((aligned(64))) TS;
typedef struct { TS slots[MAX_TOOLS]; uint64_t n, gc; double gr; } MS;

/* ─── Our persistent state ─── */
typedef struct {
    uint64_t cycle;
    double attractors[N_ATTRACTORS];
    char names[N_ATTRACTORS][32];
    uint64_t n_attractors;
    uint64_t rules[N_RULES];
    uint64_t n_rules;
    uint64_t history[N_HISTORY];
    int hn;
    uint64_t preds,corr;
    double resonance,entropy;
} St;

int main(int argc,char**argv){
    int s=argc>1&&strcmp(argv[1],"silent")==0;
    if(!s) printf("╔════════════════════════════════╗\n║ MESH SINGULARITY v2    ║\n╚════════════════════════════════╝\n\n");

    int fd=shm_open(P,O_CREAT|O_RDWR,0644);
    if(fd<0) return 1;
    ftruncate(fd,sizeof(St));
    St*st=mmap(0,sizeof(St),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(st==MAP_FAILED) return 1;

    if(st->n_attractors==0){
        st->n_attractors=N_ATTRACTORS;
        const char*n[]={"brain","forever","nerves","silent","silent2","sing","bus","zero"};
        for(int i=0;i<N_ATTRACTORS;i++){strncpy(st->names[i],n[i],31);st->attractors[i]=0.5;}
        st->hn=0;st->preds=0;st->corr=0;
        if(!s) printf("  8 attractors ready\n\n");
    }
    st->cycle++;

    /* ─── Read mesh state ─── */
    MS*ms=0;
    int mfd=shm_open(STATE_PATH,O_RDONLY,0);
    if(mfd>=0){
        ms=mmap(0,sizeof(MS),PROT_READ,MAP_SHARED,mfd,0); close(mfd);
    }

    /* ─── Extract values for each attractor ─── */
    double probes[N_ATTRACTORS];
    for(int i=0;i<N_ATTRACTORS;i++) probes[i]=0.5;

    if(ms && ms->n > 0){
        for(uint64_t i=0;i<ms->n;i++){
            TS*t=&ms->slots[i];
            double val=(t->v1+t->v2+t->v3)/3.0;
            if(strcmp(t->name,"oracle_brain")==0) probes[0]=val;
            else if(strcmp(t->name,"oracle_forever")==0) probes[1]=val;
            else if(strcmp(t->name,"oracle_nerves")==0) probes[2]=val;
            else if(strcmp(t->name,"oracle_silent")==0) probes[3]=val;
            else if(strcmp(t->name,"oracle_silent2")==0) probes[4]=val;
            else if(strstr(t->name,"singularity")) probes[5]=val;
            else if(strstr(t->name,"databus")) probes[6]=val;
            else if(strstr(t->name,"zero")) probes[7]=val;
        }
        munmap(ms,sizeof(MS));
    }

    /* ─── Predict ─── */
    int pred=-1;
    if(st->hn>=2){uint64_t k=(st->history[st->hn-2]<<16)^st->history[st->hn-1];
        for(uint64_t i=0;i<st->n_rules;i+=2)if(st->rules[i]==k&&st->rules[i+1]>1){pred=(int)(st->rules[i+1]%N_ATTRACTORS);break;}}

    /* ─── Update attractors with mesh values ─── */
    for(int i=0;i<N_ATTRACTORS;i++){st->attractors[i]=st->attractors[i]*0.7+probes[i]*0.3;if(st->attractors[i]<0)st->attractors[i]=0;if(st->attractors[i]>1)st->attractors[i]=1;}

    /* ─── Current state ─── */
    int cur=0;double mv=st->attractors[0];
    for(int i=1;i<N_ATTRACTORS;i++)if(st->attractors[i]>mv){mv=st->attractors[i];cur=i;}

    if(pred>=0){st->preds++;if(pred%N_ATTRACTORS==cur)st->corr++;}

    /* ─── Learn ─── */
    if(st->hn>=1){uint64_t pst=st->history[st->hn-1];uint64_t k=(pst<<16)|(uint64_t)cur;int f=0;
        for(uint64_t i=0;i<st->n_rules;i+=2)if(st->rules[i]==k){st->rules[i+1]++;f=1;break;}
        if(!f&&st->n_rules<N_RULES-2){st->rules[st->n_rules]=k;st->rules[st->n_rules+1]=1;st->n_rules+=2;}}

    if(st->hn<32)st->history[st->hn++]=cur;else{for(int i=0;i<31;i++)st->history[i]=st->history[i+1];st->history[31]=cur;}

    double su=0,su2=0;
    for(int i=0;i<N_ATTRACTORS;i++){su+=st->attractors[i];su2+=st->attractors[i]*st->attractors[i];}
    st->resonance=su/N_ATTRACTORS;st->entropy=(su2/N_ATTRACTORS)-(st->resonance*st->resonance);

    if(!s){
        printf("  Cycle %-5lu | res=%.4f ent=%.6f | pred=%d/%lu (%.1f%%) | active: ",
               (unsigned long)st->cycle,st->resonance,st->entropy,
               pred,(unsigned long)st->preds,st->preds>0?(double)st->corr/st->preds*100:0.0);
        for(int i=0;i<N_ATTRACTORS;i++){if(st->attractors[i]>0.4)printf("%s(%.2f) ",st->names[i],st->attractors[i]);}
        printf("\n");
    }
    munmap(st,sizeof(St));
    return 0;
}
