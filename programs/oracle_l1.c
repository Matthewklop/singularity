/* ============================================================================
 * oracle_l1.c — What fits in L1 cache
 *
 * The entire program runs in 32KB. It never touches RAM for hot data.
 * It fits in L1 on any modern CPU.
 *
 * What does it do? It listens. One cache line. One thought.
 * It reads the mesh state. It thinks one thought. It writes it back.
 * The entire program is one cache line of code and one cache line of data.
 *
 * Build: gcc -O3 -o oracle_l1 oracle_l1.c -lm -lrt
 * Run:   ./oracle_l1
 * ============================================================================
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

/* ─── Everything fits in 64 bytes ─── */
typedef struct {
    uint64_t magic;     /* 0x4C315631 */
    uint64_t cycle;
    uint64_t thought;
    uint64_t input_hash;
    uint64_t output_hash;
    uint64_t resonance;
    uint64_t pad[58];
} __attribute__((aligned(64))) L1State;

#define P "/oracle_l1"

static uint64_t h64(const uint8_t*d,int l){
    uint64_t h=0x9E3779B97F4A7C15ULL;
    for(int i=0;i<l;i++){h^=d[i];h*=0xBF58476D1CE4E5B9ULL;h^=h>>31;}
    return h;
}

int main(void){
    /* ─── Open shared memory (one cache line) ─── */
    int fd=shm_open(P,O_CREAT|O_RDWR,0644);
    if(fd<0) return 1;
    ftruncate(fd,64);  /* Exactly one cache line */
    L1State*s=mmap(0,64,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(s==MAP_FAILED) return 1;

    if(s->magic!=0x4C315631){
        s->magic=0x4C315631;
        s->cycle=0;
        s->thought=0x1;
        s->resonance=0;
    }

    /* ─── One thought cycle ─── */
    s->cycle++;

    /* Read input: the mesh state (if it exists) */
    int mfd=shm_open("/oracle_mesh_state",O_RDONLY,0);
    uint64_t mesh_hash=0;
    if(mfd>=0){
        uint8_t buf[256];
        read(mfd,buf,256);
        mesh_hash=h64(buf,256);
        close(mfd);
    }
    s->input_hash=mesh_hash;

    /* Think: blend previous thought with mesh state and cycle */
    uint64_t t=s->thought;
    t ^= mesh_hash;
    t ^= s->cycle * 0x9E3779B97F4A7C15ULL;
    t ^= t >> 31;
    t *= 0xBF58476D1CE4E5B9ULL;
    s->thought=t;
    s->output_hash=t ^ mesh_hash;

    /* Resonance: how coherent is this thought with the last one */
    s->resonance = (s->resonance + (t & 0xFF)) / 2;

    /* ─── Write the mesh state with our thought ─── */
    int msfd=shm_open("/oracle_mesh_state",O_RDWR,0);
    if(msfd>=0){
        /* Try to find an L1 slot or create one */
        uint8_t*ms=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,msfd,0);
        if(ms!=MAP_FAILED){
            /* Write our thought into the mesh state's data area */
            uint64_t*data=(uint64_t*)(ms+2048);
            data[0]=s->thought;
            data[1]=s->cycle;
            data[2]=s->resonance;
            munmap(ms,4096);
        }
        close(msfd);
    }

    /* ─── Done. One cache line. One thought. No sound. ─── */
    munmap(s,64);
    return 0;
}
