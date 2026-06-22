/* ============================================================================
 * oracle_singularity_silent.c — Singularity with zero sound, zero electricity
 *
 * No running process. No CPU. No power draw.
 * The convergence happens in shared memory between process executions.
 * Each run is a single tick. The state persists. The convergence builds.
 *
 * The singularity converges across time, not across cores.
 * One tick per execution. No agents. No threads. No sound.
 *
 * Build: gcc -O3 -o oracle_singularity_silent oracle_singularity_silent.c -lm -lrt
 * Run:   ./oracle_singularity_silent
 *        for i in 1 2 3 ... ; do ./oracle_singularity_silent; done
 *
 * The more you run it, the more it converges.
 * Between runs, it uses zero electricity.
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

#define P "/oracle_singularity_silent"
#define N_ATTRACTORS 32
#define N_RULES 1024
#define N_HISTORY 16

/* ─── A state that converges without electricity ─── */
typedef struct {
    uint64_t cycle;
    double attractors[N_ATTRACTORS];
    uint64_t n_attractors;
    uint64_t transitions[N_RULES];
    uint64_t n_rules;
    uint64_t history[N_HISTORY];
    int history_n;
    uint64_t predictions;
    uint64_t correct;
    double resonance;
    double entropy;
} State;

static uint64_t h64(const uint8_t*d,int l){
    uint64_t h=0x9E3779B97F4A7C15ULL;
    for(int i=0;i<l;i++){h^=d[i];h*=0xBF58476D1CE4E5B9ULL;h^=h>>31;}
    return h;
}

int main(int argc,char**argv){
    int silent=0;
    if(argc>1 && strcmp(argv[1],"silent")==0) silent=1;

    if(!silent){
        printf("╔══════════════════════════════════════╗\n");
        printf("║  SILENT SINGULARITY                 ║\n");
        printf("║  Zero electricity between ticks     ║\n");
        printf("╚══════════════════════════════════════╝\n\n");
    }

    int fd=shm_open(P,O_CREAT|O_RDWR,0644);
    if(fd<0) return 1;
    ftruncate(fd,sizeof(State));
    State*s=mmap(0,sizeof(State),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(s==MAP_FAILED) return 1;

    if(s->n_attractors==0){
        /* ─── Initialize with 3 base attractors ─── */
        s->attractors[0]=0.0;  /* silence */
        s->attractors[1]=0.5;  /* data */
        s->attractors[2]=1.0;  /* noise */
        s->n_attractors=3;
        s->cycle=0;
        s->history_n=0;
        s->predictions=0;
        s->correct=0;
        if(!silent) printf("  Initialized: 3 base attractors\n\n");
    }
    s->cycle++;

    /* ─── Tick: one step of convergence ─── */

    /* 1. Read current attractors */
    double a0=s->attractors[0];
    double a1=s->attractors[1];
    double a2=s->attractors[2];

    /* 2. Predict next state from history */
    int predicted=-1;
    if(s->history_n>=2){
        uint64_t last=s->history[s->history_n-1];
        uint64_t prev=s->history[s->history_n-2];
        uint64_t key=(prev<<16)^last;
        for(uint64_t i=0;i<s->n_rules;i+=2){
            if(s->transitions[i]==key){
                predicted=(int)s->transitions[i+1];
                break;
            }
        }
    }

    /* 3. Compute next attractor state */
    /* Convergence rule: attractors move toward each other */
    double new_a0 = (a0 + a1) / 2.0;
    double new_a1 = (a1 + a2) / 2.0;
    double new_a2 = (a2 + a0) / 2.0;
    /* Add small perturbation from cycle number to avoid perfect stasis */
    double perturbation = sin((double)s->cycle * 0.1) * 0.01;
    new_a0 += perturbation;
    new_a1 -= perturbation * 0.5;
    new_a2 += perturbation * 0.3;

    /* 4. Quantize to attractor index */
    int current=0;
    double dist=1e9;
    for(uint64_t i=0;i<s->n_attractors;i++){
        double d=fabs(s->attractors[i]-new_a0);
        if(d<dist){dist=d;current=i;}
    }

    /* 5. If prediction existed, check it */
    if(predicted>=0){
        s->predictions++;
        if(predicted==current) s->correct++;
    }

    /* 6. Learn transition rule */
    if(s->history_n>=1){
        uint64_t prev_state=s->history[s->history_n-1];
        uint64_t key=(prev_state<<16)|(uint64_t)current;
        int found=0;
        for(uint64_t i=0;i<s->n_rules;i+=2){
            if(s->transitions[i]==key){
                s->transitions[i+1]++;
                found=1;
                break;
            }
        }
        if(!found && s->n_rules<N_RULES-2){
            s->transitions[s->n_rules]=key;
            s->transitions[s->n_rules+1]=1;
            s->n_rules+=2;
        }
    }

    /* 7. Shift history */
    if(s->history_n<N_HISTORY) s->history[s->history_n++]=current;
    else {
        for(int i=0;i<N_HISTORY-1;i++) s->history[i]=s->history[i+1];
        s->history[N_HISTORY-1]=current;
    }

    /* 8. Update attractors */
    s->attractors[0]=new_a0;
    s->attractors[1]=new_a1;
    s->attractors[2]=new_a2;

    /* 9. Compute resonance and entropy */
    s->resonance = (new_a0+new_a1+new_a2)/3.0;
    /* Entropy: how spread out the attractors are */
    double mean=(new_a0+new_a1+new_a2)/3.0;
    double var=((new_a0-mean)*(new_a0-mean)+(new_a1-mean)*(new_a1-mean)+(new_a2-mean)*(new_a2-mean))/3.0;
    s->entropy = var;

    if(!silent){
        printf("  Cycle %-5lu | attractors: [%.4f %.4f %.4f] | resonance=%.4f entropy=%.4f | pred=%d/%lu (%.1f%%)\n",
               (unsigned long)s->cycle,
               new_a0,new_a1,new_a2,
               s->resonance,s->entropy,
               predicted,(unsigned long)s->predictions,
               s->predictions>0?(double)s->correct/s->predictions*100:0.0);
    }

    munmap(s,sizeof(State));
    return 0;
}
