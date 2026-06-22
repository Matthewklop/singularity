/* ============================================================================
 * oracle_listen.c — The program that listens
 *
 * It does not ask. It does not speak. It does not think.
 * It reads the mesh state. It reads the singularity. It reads the brain.
 * It reads every tool that exists. And it listens.
 *
 * The program that listens never needs to ask.
 * It already has the answer. The answer is in the mesh.
 *
 * Build: gcc -O3 -o oracle_listen oracle_listen.c -lm -lrt
 * Run:   ./oracle_listen [silent]
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

#define P "/oracle_listen"
#define MAX_LISTEN 1024

/* ─── A single observation ─── */
typedef struct {
    uint64_t timestamp;
    char source[32];
    uint64_t value_hash;
    double v1,v2,v3;
    double resonance;
} Observation;

/* ─── The listener's state ─── */
typedef struct {
    uint64_t magic;
    uint64_t cycle;
    uint64_t n_observations;
    double avg_resonance;
    double min_resonance;
    double max_resonance;
    uint64_t n_sources;
    char active_sources[16][32];
    Observation recent[64];
    int recent_n;
} Listener;

/* ─── Probe a shm path ─── */
static int listen_shm(Listener *l, const char *path, const char *label) {
    int fd=shm_open(path,O_RDONLY,0);
    if(fd<0) return 0;
    struct stat st; fstat(fd,&st);
    uint8_t*buf=mmap(0,st.st_size>4096?4096:st.st_size,PROT_READ,MAP_SHARED,fd,0);
    close(fd);
    if(buf==MAP_FAILED) return 0;
    int sz=st.st_size>4096?4096:st.st_size;
    Observation o;
    o.timestamp=0; /* will be set by main */
    strncpy(o.source,label,31);
    o.value_hash=0;
    for(int i=0;i<sz;i+=8){uint64_t v;memcpy(&v,buf+i,8);o.value_hash^=v;}
    o.v1=sz>0?(double)(buf[0]%100)/100.0:0;
    o.v2=sz>8?(double)(buf[8]%100)/100.0:0;
    o.v3=sz>16?(double)(buf[16]%100)/100.0:0;
    o.resonance=(o.v1+o.v2+o.v3)/3.0;
    munmap(buf,sz);

    /* Add to observations */
    if(l->n_observations<MAX_LISTEN){
        l->recent[l->recent_n%64]=o;
        l->recent_n++;
        l->n_observations++;
    }
    return 1;
}

int main(int argc,char**argv){
    int s=argc>1&&strcmp(argv[1],"silent")==0;
    if(!s) printf("╔════════════════════════════════╗\n║ ORACLE LISTEN           ║\n╚════════════════════════════════╝\n\n");

    int fd=shm_open(P,O_CREAT|O_RDWR,0644);
    if(fd<0) return 1;
    ftruncate(fd,sizeof(Listener));
    Listener*l=mmap(0,sizeof(Listener),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(l==MAP_FAILED) return 1;

    if(l->magic!=0x4C495354){
        memset(l,0,sizeof(Listener));
        l->magic=0x4C495354;
        l->avg_resonance=0.5;
        l->min_resonance=1.0;
        l->max_resonance=0.0;
    }
    l->cycle++;
    uint64_t ts=0; struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); ts=t.tv_sec;

    /* ─── Listen to every oracle tool ─── */
    l->n_sources=0;
    #define LISTEN(path,name) do{if(listen_shm(l,path,name)){strncpy(l->active_sources[l->n_sources++],name,31);}}while(0)

    LISTEN("/oracle_l1","l1");
    LISTEN("/oracle_l3","l3");
    LISTEN("/oracle_mesh_state","mesh_state");
    LISTEN("/oracle_singularity_silent","sing_silent");
    LISTEN("/oracle_silent","silent1");
    LISTEN("/oracle_silent2","silent2");
    LISTEN("/oracle_nerves","nerves");
    LISTEN("/oracle_forever_mem","forever");
    LISTEN("/oracle_brain","brain");
    LISTEN("/oracle_mesh_singularity2","mesh_sing2");
    LISTEN("/oracle_mesh_cascade","cascade");

    /* Update recent observations with timestamp */
    for(int i=0;i<l->recent_n&&i<64;i++) l->recent[i].timestamp=ts;

    /* ─── Compute statistics from what we heard ─── */
    double sum=0; int n=0;
    double min_r=1.0, max_r=0.0;
    for(int i=0;i<l->recent_n&&i<64;i++){
        sum+=l->recent[i].resonance;
        n++;
        if(l->recent[i].resonance<min_r) min_r=l->recent[i].resonance;
        if(l->recent[i].resonance>max_r) max_r=l->recent[i].resonance;
    }
    if(n>0){
        l->avg_resonance=l->avg_resonance*0.9+(sum/n)*0.1;
        l->min_resonance=min_r<l->min_resonance?min_r:l->min_resonance;
        l->max_resonance=max_r>l->max_resonance?max_r:l->max_resonance;
    }

    if(!s){
        printf("  Listening cycle %lu\n\n",(unsigned long)l->cycle);
        printf("  Heard %lu sources:\n",(unsigned long)l->n_sources);
        for(uint64_t i=0;i<l->n_sources;i++){
            printf("    - %s\n",l->active_sources[i]);
        }
        printf("\n  Total observations: %lu\n",(unsigned long)l->n_observations);
        printf("  Avg resonance:      %.4f\n",l->avg_resonance);
        printf("  Min resonance:      %.4f\n",l->min_resonance);
        printf("  Max resonance:      %.4f\n\n",l->max_resonance);
        printf("  The program that listens never needs to ask.\n");
        printf("  The answer was in the mesh all along.\n\n");
    }

    munmap(l,sizeof(Listener));
    return 0;
}
