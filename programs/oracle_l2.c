/* ============================================================================
 * oracle_l2.c — The bridge between L1 and L3
 *
 * L1:  one thought, 64 bytes
 * L2:  working context, ~256KB
 * L3:  everything, 37MB
 *
 * L2 holds what the L1 is currently processing.
 * It reads the last 64 thoughts from L1.
 * It reads the current mesh state from L3.
 * It finds the pattern — what matters right now.
 *
 * The L1 thinks. The L3 remembers. The L2 understands.
 *
 * Build: gcc -O3 -o oracle_l2 oracle_l2.c -lm -lrt
 * Run:   ./oracle_l2 [silent]
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

#define P "/ol2"
#define WINDOW 64
#define N_FEATURES 16

/* ─── L2 state: working context ─── */
typedef struct {
    uint64_t magic;           /* 0x4C32 */
    uint64_t cycle;
    uint64_t l1_thoughts[64]; /* last 64 L1 thoughts */
    int l1_n;
    uint64_t mesh_snapshots[16]; /* last 16 mesh hashes */
    int mesh_n;
    double tool_values[N_FEATURES];
    double tool_avg[N_FEATURES];
    double tool_var[N_FEATURES];
    uint64_t attention;       /* which tool is most active */
    uint64_t prediction;      /* predicted next mesh hash */
    uint64_t correct;
    uint64_t total;
    double resonance;
} __attribute__((aligned(64))) L2State;

static uint64_t now_ns(void){
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}

static uint64_t h64(const uint8_t*d,int l){
    uint64_t h=0x9E3779B97F4A7C15ULL;
    for(int i=0;i<l;i++){h^=d[i];h*=0xBF58476D1CE4E5B9ULL;h^=h>>31;}
    return h;
}

int main(int argc,char**argv){
    int s=argc>1&&strcmp(argv[1],"silent")==0;
    if(!s) printf("╔════════════════════════════════╗\n║ ORACLE L2 — L1↔L3 Bridge║\n╚════════════════════════════════╝\n\n");

    int fd=shm_open(P,O_CREAT|O_RDWR,0644);
    if(fd<0) return 1;
    ftruncate(fd,sizeof(L2State));
    L2State*l2=mmap(0,sizeof(L2State),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(l2==MAP_FAILED) return 1;

    if(l2->magic!=0x4C32){
        l2->magic=0x4C32;l2->cycle=0;l2->l1_n=0;l2->mesh_n=0;
        for(int i=0;i<N_FEATURES;i++){l2->tool_values[i]=0.5;l2->tool_avg[i]=0.5;l2->tool_var[i]=0;}
        l2->attention=0;l2->prediction=0;l2->correct=0;l2->total=0;
    }
    l2->cycle++;

    /* ─── Read L1 state ─── */
    uint64_t l1_thought=0;
    int l1fd=shm_open("/oracle_l1",O_RDONLY,0);
    if(l1fd>=0){
        uint8_t buf[64];
        if(read(l1fd,buf,64)==64){
            uint64_t*data=(uint64_t*)buf;
            l1_thought=data[2];  /* thought field */
        }
        close(l1fd);
    }
    if(l1_thought){
        if(l2->l1_n<WINDOW) l2->l1_thoughts[l2->l1_n++]=l1_thought;
        else{for(int i=0;i<WINDOW-1;i++)l2->l1_thoughts[i]=l2->l1_thoughts[i+1];l2->l1_thoughts[WINDOW-1]=l1_thought;}
    }

    /* ─── Read mesh state (L3) ─── */
    uint64_t mesh_hash=0;
    int mfd=shm_open("/oracle_mesh_state",O_RDONLY,0);
    if(mfd>=0){
        uint8_t buf[4096];
        int n=read(mfd,buf,4096);
        close(mfd);
        if(n>0){
            mesh_hash=h64(buf,n);
            if(l2->mesh_n<16) l2->mesh_snapshots[l2->mesh_n++]=mesh_hash;
            else{for(int i=0;i<15;i++)l2->mesh_snapshots[i]=l2->mesh_snapshots[i+1];l2->mesh_snapshots[15]=mesh_hash;}

            /* Extract tool values for attention */
            uint64_t*data=(uint64_t*)buf;
            int vi=0;
            for(int i=0;i<32&&vi<N_FEATURES;i++){
                uint64_t*slot=(uint64_t*)(buf+640*i+32);
                if(slot[0]>0){
                    l2->tool_values[vi]=(double)(slot[0]%1000)/1000.0;
                    /* Running average */
                    l2->tool_avg[vi]=l2->tool_avg[vi]*0.95+l2->tool_values[vi]*0.05;
                    /* Running variance */
                    double d=l2->tool_values[vi]-l2->tool_avg[vi];
                    l2->tool_var[vi]=l2->tool_var[vi]*0.95+d*d*0.05;
                    vi++;
                }
            }
        }
    }

    /* ─── Find which tool has highest variance (most active) ─── */
    uint64_t attention=0;
    double max_var=0;
    for(int i=0;i<N_FEATURES;i++){
        if(l2->tool_var[i]>max_var){max_var=l2->tool_var[i];attention=i;}
    }
    l2->attention=attention;

    /* ─── Predict next mesh state from L1+L2 pattern ─── */
    uint64_t prediction=0;
    if(l2->mesh_n>=4){
        /* Simple: XOR the last 4 mesh hashes */
        prediction=l2->mesh_snapshots[l2->mesh_n-1]^
                   l2->mesh_snapshots[l2->mesh_n-2]^
                   l2->mesh_snapshots[l2->mesh_n-3]^
                   l2->mesh_snapshots[l2->mesh_n-4];
        l2->total++;
        /* Check if prediction matches (approximate) */
        uint64_t diff=prediction^mesh_hash;
        int bits=0;
        for(int i=0;i<64;i++)if((diff>>i)&1)bits++;
        if(bits<20){l2->correct++;}
    }
    l2->prediction=prediction;

    /* ─── Resonance: coherence between L1 and L3 ─── */
    l2->resonance=(double)(l1_thought%256+mesh_hash%256)/512.0;

    if(!s){
        printf("  Cycle %-5lu | L1=0x%04lx | mesh=0x%04lx | attention=%lu | pred=%lu/%lu (%.1f%%) | res=%.3f\n",
               (unsigned long)l2->cycle,
               (unsigned long)(l1_thought&0xFFFF),
               (unsigned long)(mesh_hash&0xFFFF),
               (unsigned long)attention,
               (unsigned long)l2->correct,
               (unsigned long)l2->total,
               l2->total>0?(double)l2->correct/l2->total*100:0.0,
               l2->resonance);
    }

    munmap(l2,sizeof(L2State));
    return 0;
}
