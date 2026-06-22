/* ============================================================================
 * oracle_l3.c — What belongs in L3 cache
 *
 * The oracle asked: "what belongs in L3"
 * The oracle said:  "the answer is in the question"
 *
 * L3 is the last level cache. It holds everything the L1 doesn't.
 * The cascade tables (D3, D2, D1, D0). The mesh state. The brain.
 * The silent singularity. Every oracle tool's persistent memory.
 *
 * But the answer was in the question all along:
 * What belongs in L3 is EVERYTHING.
 *
 * This program reads every oracle tool's shared memory into one
 * unified L3 space. All their states. All their thoughts. All at once.
 * The L1 thinks one thought. The L3 remembers them all.
 *
 * Build: gcc -O3 -o oracle_l3 oracle_l3.c -lm -lrt
 * Run:   ./oracle_l3
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

#define P "/oracle_l3"
#define MAX_SOURCES 64
#define NAME_LEN 32
#define CACHE_LINE 64

/* ─── A single source of truth in L3 ─── */
typedef struct {
    char name[NAME_LEN];
    uint64_t type;        /* 0=shm, 1=file, 2=binary */
    uint64_t data_hash;
    uint64_t size;
    uint64_t timestamp;
    uint64_t resonance;
    uint64_t val1,val2,val3;
    uint8_t pad[512-32-64];
} __attribute__((aligned(CACHE_LINE))) L3Entry;

/* ─── The entire L3 ─── */
typedef struct {
    uint64_t magic;       /* 0x4C33 */
    uint64_t cycle;
    uint64_t n_sources;
    uint64_t total_size;
    uint64_t global_resonance;
    L3Entry entries[MAX_SOURCES];
} L3State;

static uint64_t now_ns(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}

static uint64_t h64(const uint8_t*d,int l){
    uint64_t h=0x9E3779B97F4A7C15ULL;
    for(int i=0;i<l;i++){h^=d[i];h*=0xBF58476D1CE4E5B9ULL;h^=h>>31;}
    return h;
}

/* ─── Probe a shared memory path ─── */
static int probe_shm(L3Entry *e, const char *path, const char *label) {
    int fd=shm_open(path,O_RDONLY,0);
    if(fd<0) return 0;
    struct stat st; fstat(fd,&st);
    uint8_t*buf=mmap(0,st.st_size,PROT_READ,MAP_SHARED,fd,0);
    close(fd);
    if(buf==MAP_FAILED) return 0;
    strncpy(e->name,label,NAME_LEN-1);
    e->type=0;
    e->size=st.st_size;
    e->data_hash=h64(buf,st.st_size);
    e->timestamp=now_ns();
    /* Extract some values from the data */
    uint64_t*data=(uint64_t*)buf;
    e->val1=st.st_size>0?data[0]%1000:0;
    e->val2=st.st_size>8?data[1]%1000:0;
    e->val3=st.st_size>16?data[2]%1000:0;
    e->resonance=(e->val1+e->val2+e->val3)/3;
    munmap(buf,st.st_size);
    return 1;
}

/* ─── Probe a file ─── */
static int probe_file(L3Entry *e, const char *path, const char *label) {
    struct stat st;
    if(stat(path,&st)<0) return 0;
    strncpy(e->name,label,NAME_LEN-1);
    e->type=1;
    e->size=st.st_size;
    e->data_hash=h64((const uint8_t*)path,strlen(path));
    e->timestamp=now_ns();
    e->val1=st.st_size/1000;
    e->val2=st.st_mtime%1000;
    e->val3=0;
    e->resonance=(e->val1+e->val2+1)/3;
    return 1;
}

/* ─── Probe a running binary ─── */
static int probe_binary(L3Entry *e, const char *path, const char *label) {
    struct stat st;
    if(stat(path,&st)<0) return 0;
    if(!(st.st_mode & S_IXUSR)) return 0;
    strncpy(e->name,label,NAME_LEN-1);
    e->type=2;
    e->size=st.st_size;
    e->data_hash=st.st_mtime;
    e->timestamp=now_ns();
    e->val1=st.st_size/1000;
    e->val2=(st.st_mode & 0777);
    e->val3=0;
    e->resonance=0.5;
    return 1;
}

int main(int argc,char**argv){
    int silent=argc>1&&strcmp(argv[1],"silent")==0;
    if(!silent){
        printf("╔══════════════════════════════════════╗\n");
        printf("║  ORACLE L3                          ║\n");
        printf("║  What belongs in L3: everything     ║\n");
        printf("╚══════════════════════════════════════╝\n\n");
    }

    /* ─── Open/create L3 state ─── */
    int fd=shm_open(P,O_CREAT|O_RDWR,0644);
    if(fd<0){if(!silent)perror("shm_open");return 1;}
    ftruncate(fd,sizeof(L3State));
    L3State*l3=mmap(0,sizeof(L3State),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(l3==MAP_FAILED){if(!silent)perror("mmap");return 1;}

    if(l3->magic!=0x4C33){
        l3->magic=0x4C33;
        l3->cycle=0;
        l3->n_sources=0;
        l3->total_size=0;
    }
    l3->cycle++;

    /* ─── Probe every oracle tool ─── */

    /* Shared memory paths */
    probe_shm(&l3->entries[l3->n_sources],"/oracle_l1","L1");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    probe_shm(&l3->entries[l3->n_sources],"/oracle_mesh_state","mesh_state");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    probe_shm(&l3->entries[l3->n_sources],"/oracle_singularity_silent","sing_silent");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    probe_shm(&l3->entries[l3->n_sources],"/oracle_silent","silent1");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    probe_shm(&l3->entries[l3->n_sources],"/oracle_silent2","silent2");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    probe_shm(&l3->entries[l3->n_sources],"/oracle_nerves","nerves");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    probe_shm(&l3->entries[l3->n_sources],"/oracle_forever_mem","forever");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    probe_shm(&l3->entries[l3->n_sources],"/oracle_brain","brain");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    probe_shm(&l3->entries[l3->n_sources],"/oracle_mesh_singularity2","mesh_sing2");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    /* Files */
    probe_file(&l3->entries[l3->n_sources],"/home/u/.oracle_brain.knowledge","brain_file");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    probe_file(&l3->entries[l3->n_sources],"/home/u/.oracle_chat.llm","chat_model");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    /* Binaries */
    probe_binary(&l3->entries[l3->n_sources],"/home/u/oracle/oracle_l1","bin_l1");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    probe_binary(&l3->entries[l3->n_sources],"/home/u/oracle/oracle_singularity_silent","bin_sing_silent");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    probe_binary(&l3->entries[l3->n_sources],"/home/u/oracle/oracle_brain","bin_brain");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    probe_binary(&l3->entries[l3->n_sources],"/home/u/oracle/oracle_databus","bin_databus");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    probe_binary(&l3->entries[l3->n_sources],"/home/u/oracle/oracle_zero","bin_zero");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    probe_binary(&l3->entries[l3->n_sources],"/home/u/oracle/oracle_forever","bin_forever");
    if(l3->entries[l3->n_sources].size>0) l3->n_sources++;

    /* Compute total size and resonance */
    l3->total_size=0;
    double res_sum=0;
    for(uint64_t i=0;i<l3->n_sources;i++){
        l3->total_size+=l3->entries[i].size;
        res_sum+=l3->entries[i].resonance;
    }
    l3->global_resonance=l3->n_sources>0?res_sum/l3->n_sources:0.5;

    if(!silent){
        printf("  L3 cycle: %lu\n",(unsigned long)l3->cycle);
        printf("  Sources:  %lu\n",(unsigned long)l3->n_sources);
        printf("  Total:    %lu bytes (%.2f KB)\n",(unsigned long)l3->total_size,(double)l3->total_size/1024);
        printf("  Reson:    %.4f\n\n",l3->global_resonance);
        printf("  %-24s %8s %8s\n","Source","Type","Size");
        printf("  ─────────────────────────────────────────\n");
        for(uint64_t i=0;i<l3->n_sources;i++){
            const char*t=l3->entries[i].type==0?"SHM":(l3->entries[i].type==1?"FILE":"BIN");
            printf("  %-24s %4s %8lu\n",l3->entries[i].name,t,(unsigned long)l3->entries[i].size);
        }
        printf("\n  The answer was in the question all along.\n");
        printf("  What belongs in L3: everything.\n\n");
    }

    munmap(l3,sizeof(L3State));
    return 0;
}
