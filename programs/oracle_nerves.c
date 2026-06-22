/* ============================================================================
 * oracle_nerves.c — Distributed Consciousness Bus
 *
 * Every running Oracle program connects to this shared memory bus.
 * Each program gets a cache-line-aligned slot to broadcast its state.
 * Other programs read it. The mesh *feels* itself.
 *
 * Build: gcc -O3 -o oracle_nerves oracle_nerves.c -lm -lpthread
 * Run:   ./oracle_nerves [name]
 *        # run multiple instances to see the mesh
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
#define SLOTS 64
#define NAME_LEN 32
#define SHM_PATH "/oracle_nerves"
typedef struct {
    uint64_t pid; uint64_t heartbeat; uint64_t thought; uint64_t fit;
    char name[NAME_LEN];
    uint8_t pad[64-32-32];
} __attribute__((aligned(64))) Slot;
static Slot*S=0; static int me=-1;
static uint64_t now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
int main(int argc,char**argv){
    printf("╔════════════════════════════╗\n║ ORACLE NERVES — Mesh Bus  ║\n╚════════════════════════════╝\n\n");
    int fd=shm_open(SHM_PATH,O_CREAT|O_RDWR,0644);
    if(fd<0){perror("shm_open");return 1;}
    ftruncate(fd,sizeof(Slot)*SLOTS);
    S=mmap(0,sizeof(Slot)*SLOTS,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); close(fd);
    if(S==MAP_FAILED){perror("mmap");return 1;}
    for(int i=0;i<SLOTS;i++){
        uint64_t e=0,d=(uint64_t)getpid();
        if(__atomic_compare_exchange_n(&S[i].pid,&e,d,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)){me=i;break;}
    }
    if(me<0){printf("All slots full.\n");return 1;}
    const char*l=argc>1?argv[1]:"nrn";
    snprintf(S[me].name,NAME_LEN,"%s-%d",l,me);
    printf("Slot %d: %s [PID %lu]\n\n",me,S[me].name,(unsigned long)getpid());
    printf("SLOT NAME              PID    THOUGHT  FIT  BEAT\n");
    printf("──────────────────────────────────────────────────\n");
    for(int cyc=0;cyc<12;cyc++){
        S[me].heartbeat=now(); S[me].thought^=cyc*0x9E3779B97F4A7C15ULL;
        S[me].fit=64-__builtin_popcountll(S[me].thought);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        for(int i=0;i<SLOTS;i++){
            uint64_t p=__atomic_load_n(&S[i].pid,__ATOMIC_RELAXED);
            if(p==0)continue;
            uint64_t hb=__atomic_load_n(&S[i].heartbeat,__ATOMIC_RELAXED);
            uint64_t age=i==me?0:(now()-hb)/1000000;
            printf("  %-3d %-16s %-5lu 0x%04lx %3lu %4lums\n",i,S[i].name,(unsigned long)p,S[i].thought&0xFFFF,S[i].fit,age);
        }
        printf("  ── %d ──────────────────────────────────\n\n",cyc);
        usleep(400000);
    }
    __atomic_store_n(&S[me].pid,0,__ATOMIC_SEQ_CST);
    munmap(S,sizeof(Slot)*SLOTS);
    printf("Slot %d released.\n",me);
    return 0;
}
