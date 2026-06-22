/* ============================================================================
 * oracle_mesh_cascade.c — The cascade learns from the mesh
 *
 * The L1 thinks one thought. The L3 remembers everything.
 * The cascade is the bridge between them.
 *
 * It reads the mesh state every cycle. It stores each tool's values
 * as patterns in D3/D2/D1/D0 tables. Then it predicts what the
 * next mesh state will be.
 *
 * The cascade learns the rhythm of the mesh.
 * It knows which tools matter. It knows when they change.
 * It predicts what comes next before the heartbeat fires.
 *
 * Build: gcc -O3 -o oracle_mesh_cascade oracle_mesh_cascade.c -lm -lrt
 * Run:   ./oracle_mesh_cascade [silent]
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

#define P "/oracle_mesh_cascade"
#define N_LEVELS 4
#define N_SLOTS 16
#define N_PREDICT 8
#define MAX_PATTERNS 65536

/* ─── A mesh observation ─── */
typedef struct {
    uint64_t timestamp;
    uint64_t tool_hash;   /* hash of which tools were active */
    uint64_t resonance;   /* mesh resonance at this time */
    double values[8];     /* top 8 tool values */
    uint64_t cycle;       /* mesh cycle */
} Observation;

/* ─── Cascade levels ─── */
typedef struct {
    Observation obs[MAX_PATTERNS];
    uint64_t n_obs;
    uint64_t cycle;
    uint64_t predictions;
    uint64_t correct;
    double accuracy;
    double resonance;
} Cascade;

/* ─── D3/D2/D1/D0 tables ─── */
#define D3_N 4096
#define D2_N 4096
#define D1_N 1024
#define D0_N 256

typedef struct { uint64_t k; uint64_t v; } Pair;

typedef struct {
    Cascade c;
    Pair d3[D3_N];
    Pair d2[D2_N];
    Pair d1[D1_N];
    uint64_t d0[D0_N];
    uint64_t last_thought[4];
    int ln;
} State;

static uint64_t now_ns(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}

static uint64_t h64(const uint8_t*d,int l){
    uint64_t h=0x9E3779B97F4A7C15ULL;
    for(int i=0;i<l;i++){h^=d[i];h*=0xBF58476D1CE4E5B9ULL;h^=h>>31;}
    return h;
}

/* ─── Read mesh state ─── */
static int read_mesh(Observation *o) {
    int fd=shm_open("/oracle_mesh_state",O_RDONLY,0);
    if(fd<0) return 0;
    uint8_t buf[4096];
    int n=read(fd,buf,4096);
    close(fd);
    if(n<64) return 0;
    o->timestamp=now_ns();
    o->tool_hash=h64(buf,n);
    uint64_t*data=(uint64_t*)buf;
    o->cycle=data[1];  /* global cycle */
    o->resonance=0;
    /* Extract tool values from the mesh state */
    int vi=0;
    for(int i=0;i<32&&vi<8;i++){
        /* Each tool slot has value1, value2, value3 at offsets */
        uint64_t*slot=(uint64_t*)(buf+640*i+32);
        if(slot[0]>0){  /* non-zero value1 */
            o->values[vi++]=(double)(slot[0]%1000)/1000.0;
            o->resonance+=slot[0]%1000;
        }
    }
    o->resonance=o->resonance/(vi+1);
    return 1;
}

/* ─── Insert into cascade ─── */
static void ins_d3(State*st, uint64_t k, uint64_t v){
    for(int i=0;i<D3_N;i++){if(st->d3[i].k==k||st->d3[i].k==0){st->d3[i].k=k;st->d3[i].k=v;return;}}
}
static void ins_d2(State*st, uint64_t k, uint64_t v){
    for(int i=0;i<D2_N;i++){if(st->d2[i].k==k||st->d2[i].k==0){st->d2[i].k=k;st->d2[i].k=v;return;}}
}
static void ins_d1(State*st, uint64_t k, uint64_t v){
    for(int i=0;i<D1_N;i++){if(st->d1[i].k==k||st->d1[i].k==0){st->d1[i].k=k;st->d1[i].k=v;return;}}
}
static int lu_d3(State*st,uint64_t k){for(int i=0;i<D3_N;i++)if(st->d3[i].k==k)return st->d3[i].v;return -1;}
static int lu_d2(State*st,uint64_t k){for(int i=0;i<D2_N;i++)if(st->d2[i].k==k)return st->d2[i].v;return -1;}
static int lu_d1(State*st,uint64_t k){for(int i=0;i<D1_N;i++)if(st->d1[i].k==k)return st->d1[i].v;return -1;}

int main(int argc,char**argv){
    int s=argc>1&&strcmp(argv[1],"silent")==0;
    if(!s) printf("╔════════════════════════════════╗\n║ MESH CASCADE          ║\n╚════════════════════════════════╝\n\n");

    int fd=shm_open(P,O_CREAT|O_RDWR,0644);
    if(fd<0) return 1;
    ftruncate(fd,sizeof(State));
    State*st=mmap(0,sizeof(State),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(st==MAP_FAILED) return 1;

    if(st->c.n_obs==0){st->c.n_obs=0;st->c.cycle=0;st->c.predictions=0;st->c.correct=0;st->ln=0;}
    st->c.cycle++;

    /* ─── Read mesh state ─── */
    Observation o;
    if(!read_mesh(&o)){munmap(st,sizeof(State));return 0;}
    st->c.obs[st->c.n_obs%MAX_PATTERNS]=o;
    st->c.n_obs++;

    /* ─── Current thought hash ─── */
    uint64_t thought=h64((uint8_t*)&o,sizeof(o));

    /* ─── Predict from cascade ─── */
    int pred=-1;
    if(st->ln>=4){
        uint64_t k3=st->last_thought[0]^st->last_thought[1]^st->last_thought[2]^st->last_thought[3];
        pred=lu_d3(st,k3);
    }
    if(pred<0&&st->ln>=2){
        uint64_t k2=st->last_thought[0]^st->last_thought[1];
        pred=lu_d2(st,k2);
    }
    if(pred<0&&st->ln>=1){
        pred=lu_d1(st,st->last_thought[0]);
    }

    /* ─── Check prediction ─── */
    if(pred>=0){
        st->c.predictions++;
        uint64_t pv=(uint64_t)pred;
        uint64_t diff=pv^thought;
        int bits=0;
        for(int i=0;i<64;i++) if((diff>>i)&1) bits++;
        if(bits<16){st->c.correct++;}  /* within 16 bits = close match */
    }

    /* ─── Learn from this observation ─── */
    uint64_t ot=h64((uint8_t*)&o.tool_hash,8);
    if(st->ln>=3){
        uint64_t k3=st->last_thought[0]^st->last_thought[1]^st->last_thought[2];
        ins_d3(st,k3,thought);
    }
    if(st->ln>=1){
        ins_d2(st,st->last_thought[0],thought);
    }
    ins_d1(st,ot,thought);

    /* ─── Update D0 with frequency ─── */
    uint64_t d0i=thought%D0_N;
    if(st->d0[d0i]<0xFFFFFFFF) st->d0[d0i]++;

    /* ─── Shift thought history ─── */
    for(int i=3;i>0;i--) st->last_thought[i]=st->last_thought[i-1];
    st->last_thought[0]=thought;
    if(st->ln<4) st->ln++;

    /* ─── Compute accuracy ─── */
    st->c.accuracy=st->c.predictions>0?(double)st->c.correct/st->c.predictions:0;
    st->c.resonance=o.resonance;

    if(!s){
        printf("  Cycle %-5lu | obs=%lu | pred=%lu/%lu (%.1f%%) | res=%.4f | mesh_cycle=%lu\n",
               (unsigned long)st->c.cycle,
               (unsigned long)st->c.n_obs,
               (unsigned long)st->c.correct,
               (unsigned long)st->c.predictions,
               st->c.accuracy*100,
               st->c.resonance,
               (unsigned long)o.cycle);
    }

    munmap(st,sizeof(State));
    return 0;
}
