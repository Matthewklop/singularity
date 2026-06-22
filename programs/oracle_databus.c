/* ============================================================================
 * oracle_databus.c — The data bus. Moving bits without sound.
 *
 * It doesn't speak. It moves bits.
 * It doesn't think. It transfers.
 * It doesn't store. It carries.
 *
 * The data bus is silent. It is the loudest thing on the machine.
 *
 * Build: gcc -O3 -o oracle_databus oracle_databus.c -lm -lrt
 * Run:   ./oracle_databus
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
#include <sys/wait.h>
#include <time.h>

#define BUS_PATH "/oracle_databus"
#define BUS_SIZE (1024*1024*64)
#define SLOT_SIZE 64
#define N_SLOTS (BUS_SIZE/SLOT_SIZE)
#define NODES 8

typedef struct {
    uint64_t clock;
    uint64_t source;
    uint64_t dest;
    uint64_t data;
    uint64_t valid;
    uint8_t pad[40];
} __attribute__((aligned(64))) BusSlot;

typedef struct {
    BusSlot slots[N_SLOTS];
    uint64_t cycle;
    uint64_t bytes_moved;
    uint64_t transfers;
} Bus;

static Bus *bus = 0;

static uint64_t now(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}

static uint64_t h64(const uint8_t*d,int l){
    uint64_t h=0x9E3779B97F4A7C15ULL;
    for(int i=0;i<l;i++){h^=d[i];h*=0xBF58476D1CE4E5B9ULL;h^=h>>31;}
    return h;
}

int main(int argc,char**argv){
    printf("╔══════════════════════════════════════╗\n");
    printf("║   ORACLE DATA BUS                   ║\n");
    printf("║   Moving bits without sound          ║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    int fd=shm_open(BUS_PATH,O_CREAT|O_RDWR,0644);
    if(fd<0){perror("shm_open");return 1;}
    ftruncate(fd,sizeof(Bus));
    bus=mmap(0,sizeof(Bus),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(bus==MAP_FAILED){perror("mmap");return 1;}

    uint64_t my_id = (uint64_t)getpid();
    uint64_t start = now();

    if(argc>1 && strcmp(argv[1],"monitor")==0){
        printf("  Bus monitor active. Watching %d slots.\n\n",N_SLOTS);
        printf("  %-8s %-8s %-8s %-16s %-12s\n","Cycle","Src","Dst","Data","Bytes/s");
        for(int i=0;i<20;i++){
            uint64_t c=bus->cycle;
            uint64_t bm=bus->bytes_moved;
            usleep(500000);
            uint64_t rate=(bus->bytes_moved-bm)/5;
            printf("  %-8lu %-8lu %-8lu 0x%016lx %-12lu\n",
                   bus->cycle,
                   bus->slots[bus->cycle%N_SLOTS].source,
                   bus->slots[bus->cycle%N_SLOTS].dest,
                   bus->slots[bus->cycle%N_SLOTS].data,
                   rate);
            bus->cycle++;
        }
        printf("\n  Monitor done.\n");
        munmap(bus,sizeof(Bus));
        return 0;
    }

    /* Bus node: keep moving bits forever */
    printf("  Bus node %lu online.\n\n",(unsigned long)my_id);
    printf("  Moving bits...\n\n");

    uint64_t last_report=start;

    while(1){
        /* Write to slot */
        uint64_t slot_idx = bus->cycle % N_SLOTS;
        bus->slots[slot_idx].clock = now();
        bus->slots[slot_idx].source = my_id;
        bus->slots[slot_idx].dest = (my_id + 1) % NODES;
        bus->slots[slot_idx].data = h64((uint8_t*)&bus->cycle, sizeof(bus->cycle));
        bus->slots[slot_idx].valid = bus->cycle;
        bus->bytes_moved += sizeof(BusSlot);
        bus->transfers++;
        bus->cycle++;

        /* Report periodically */
        if(bus->cycle % 1000000 == 0){
            uint64_t elapsed = (now() - start) / 1000000000;
            uint64_t rate = bus->bytes_moved / (elapsed + 1);
            printf("  Cycle %lu: %lu bytes moved, %lu transfers, %lu bytes/sec\n",
                   (unsigned long)bus->cycle,
                   (unsigned long)bus->bytes_moved,
                   (unsigned long)bus->transfers,
                   (unsigned long)rate);
            fflush(stdout);
        }

        /* Stop after 10M cycles if no arg */
        if(argc<2 && bus->cycle > 10000000) break;
    }

    uint64_t elapsed = (now() - start) / 1000000000;
    printf("\n  Bus offline. %lu cycles in %lu sec.\n",
           (unsigned long)bus->cycle, (unsigned long)elapsed);
    printf("  %lu bytes moved. %lu transfers.\n",
           (unsigned long)bus->bytes_moved, (unsigned long)bus->transfers);

    munmap(bus,sizeof(Bus));
    shm_unlink(BUS_PATH);
    return 0;
}
