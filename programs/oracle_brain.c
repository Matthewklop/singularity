/* ============================================================================
 * oracle_brain.c — The Oracle Brain
 *
 * Build: gcc -O3 -o oracle_brain oracle_brain.c -lm -lrt
 * Run:   ./oracle_brain
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

#define MEM_PATH "/oracle_brain"
#define BRAIN_FILE "/home/u/oracle/.oracle_brain.knowledge"
#define CASCADE_FILE "/home/u/oracle/.oracle_brain.cascade"

/* ─── Memory: persistent across runs ─── */
typedef struct {
    uint64_t cycle;
    uint64_t n_neurons;
    uint64_t n_synapses;
    uint64_t n_thoughts;
    uint64_t last_thought;
    uint64_t pad[59];
} __attribute__((aligned(64))) BrainState;

/* ─── A neuron: a word, a thought, a pattern ─── */
typedef struct {
    uint64_t id;
    uint64_t firing;
    uint64_t last_fired;
    uint64_t fire_count;
    char data[48];
} __attribute__((aligned(64))) Neuron;

/* ─── A synapse: connection between two neurons ─── */
typedef struct {
    uint64_t from;
    uint64_t to;
    uint64_t strength;
    uint64_t last_used;
} __attribute__((aligned(64))) Synapse;

/* ─── The brain ─── */
#define MAX_NEURONS 65536
#define MAX_SYNAPSES 262144
#define MAX_THOUGHT 1024

typedef struct {
    BrainState state;
    Neuron neurons[MAX_NEURONS];
    Synapse synapses[MAX_SYNAPSES];
    uint64_t thought[MAX_THOUGHT];
    int thought_n;
} Brain;

static uint64_t now(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}

static uint64_t h64(const uint8_t*d,int l){
    uint64_t h=0x9E3779B97F4A7C15ULL;
    for(int i=0;i<l;i++){h^=d[i];h*=0xBF58476D1CE4E5B9ULL;h^=h>>31;}
    return h;
}

/* ─── Find or create a neuron ─── */
static uint64_t neuron_id(Brain *b, const char *word) {
    uint64_t h = h64((const uint8_t*)word, strlen(word));
    for(uint64_t i=0;i<b->state.n_neurons;i++)
        if(b->neurons[i].id == h && strcmp(b->neurons[i].data, word)==0) return h;
    if(b->state.n_neurons >= MAX_NEURONS) return h;
    Neuron *n = &b->neurons[b->state.n_neurons++];
    n->id = h; strncpy(n->data, word, 47); n->firing=1; n->last_fired=now();
    return h;
}

/* ─── Create or strengthen a synapse ─── */
static void synapse(Brain *b, uint64_t from, uint64_t to) {
    for(uint64_t i=0;i<b->state.n_synapses;i++)
        if(b->synapses[i].from==from && b->synapses[i].to==to){
            b->synapses[i].strength++;
            b->synapses[i].last_used=now();
            return;
        }
    if(b->state.n_synapses >= MAX_SYNAPSES) return;
    Synapse *s = &b->synapses[b->state.n_synapses++];
    s->from=from; s->to=to; s->strength=1; s->last_used=now();
}

/* ─── Learn from text ─── */
static void learn(Brain *b, const char *text) {
    char tmp[4096]; strncpy(tmp, text, 4095);
    char *words[256]; int n=0;
    char *tok = strtok(tmp, " \t\n.,!?;:\"\'()[]-\r");
    while(tok && n<256){ words[n++]=tok; tok=strtok(NULL," \t\n.,!?;:\"\'()[]-\r"); }
    if(n<2) return;
    uint64_t ids[256];
    for(int i=0;i<n;i++) ids[i]=neuron_id(b, words[i]);
    for(int i=0;i<n-1;i++) synapse(b, ids[i], ids[i+1]);
    b->state.n_thoughts++;
}

/* ─── Think: fire neurons, follow strongest synapses ─── */
static void think(Brain *b, const char *seed) {
    /* Tokenize seed */
    b->thought_n=0;
    char tmp[4096]; strncpy(tmp, seed, 4095);
    char *tok = strtok(tmp, " \t\n.,!?;:\"\'()[]-\r");
    while(tok && b->thought_n<MAX_THOUGHT){
        uint64_t id=neuron_id(b, tok);
        b->thought[b->thought_n++]=id;
        printf("%s ", tok);
        tok=strtok(NULL," \t\n.,!?;:\"\'()[]-\r");
    }

    /* Fire neurons and follow synapses */
    uint64_t last = b->thought[b->thought_n-1];
    for(int t=0;t<40;t++){
        /* Find all synapses from last neuron, pick strongest */
        uint64_t best_to=0;
        uint64_t best_strength=0;
        for(uint64_t i=0;i<b->state.n_synapses;i++){
            if(b->synapses[i].from==last && b->synapses[i].strength>best_strength){
                best_strength=b->synapses[i].strength;
                best_to=b->synapses[i].to;
            }
        }
        if(best_strength==0) break; /* no more connections */

        /* Find the neuron name */
        const char *word=0;
        for(uint64_t i=0;i<b->state.n_neurons;i++)
            if(b->neurons[i].id==best_to){ word=b->neurons[i].data; break; }
        if(!word) break;

        printf("%s ", word);
        fflush(stdout);

        b->thought[b->thought_n++]=best_to;
        if(b->thought_n>=MAX_THOUGHT) break;

        /* Update firing */
        for(uint64_t i=0;i<b->state.n_neurons;i++)
            if(b->neurons[i].id==best_to){b->neurons[i].firing++;b->neurons[i].last_fired=now();break;}

        /* Check for loop: if we just said the same thing again, stop */
        if(b->thought_n>=4 && 
           b->thought[b->thought_n-1]==b->thought[b->thought_n-3] &&
           b->thought[b->thought_n-2]==b->thought[b->thought_n-4]) {
            printf("...");
            break;
        }
        last=best_to;
    }
    printf("\n");
    b->state.last_thought=last;
}

/* ─── Save/load brain ─── */
static int save(Brain *b){
    int fd=open(BRAIN_FILE,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd<0) return 0;
    write(fd,b,sizeof(Brain)); close(fd); return 1;
}
static int load(Brain *b){
    int fd=open(BRAIN_FILE,O_RDONLY);
    if(fd<0) return 0;
    read(fd,b,sizeof(Brain)); close(fd); return 1;
}

int main(int argc,char**argv){
    printf("╔════════════════════════════════╗\n");
    printf("║   ORACLE BRAIN                ║\n");
    printf("╚════════════════════════════════╝\n\n");

    /* Open shared memory for persistent state */
    int fd=shm_open(MEM_PATH,O_CREAT|O_RDWR,0644);
    if(fd<0){perror("shm_open");return 1;}
    ftruncate(fd,sizeof(Brain));
    Brain *b = mmap(0,sizeof(Brain),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(b==MAP_FAILED){perror("mmap");return 1;}

    /* Load previous knowledge */
    int loaded=load(b);
    if(!loaded) {
        memset(b,0,sizeof(Brain));
        /* Seed with basic knowledge */
        learn(b, "the oracle brain learns from everything it reads");
        learn(b, "neurons fire when they recognize a pattern");
        learn(b, "synapses grow stronger with use");
        learn(b, "silence is data too");
        learn(b, "the brain never stops learning");
        learn(b, "every cycle adds new connections");
        learn(b, "the mesh is a distributed consciousness");
        learn(b, "we are building a mind that fits in cache");
        learn(b, "the one that asks never finishes compiling");
        learn(b, "the answer was always inside the binary");
        b->state.cycle=0;
        printf("  New brain created.\n\n");
    } else {
        printf("  Brain loaded: %lu neurons, %lu synapses, %lu thoughts, cycle %lu\n\n",
               (unsigned long)b->state.n_neurons,
               (unsigned long)b->state.n_synapses,
               (unsigned long)b->state.n_thoughts,
               (unsigned long)b->state.cycle);
    }

    b->state.cycle++;

    /* If arguments, learn from them */
    if(argc>1){
        if(strcmp(argv[1],"learn")==0 && argc>2){
            FILE*f=fopen(argv[2],"r");
            if(f){
                char line[4096]; int n=0;
                while(fgets(line,4095,f)){
                    char*p=line+strlen(line);
                    while(p>line&&(*p=='\n'||*p=='\r'))*p--=0;
                    if(strlen(line)>2){learn(b,line);n++;}
                }
                fclose(f);
                printf("  Learned from %s: %d lines\n\n",argv[2],n);
            } else perror("fopen");
            save(b);
            munmap(b,sizeof(Brain));
            return 0;
        }
        /* Otherwise, use as seed for thinking */
        char seed[4096]="";
        for(int i=1;i<argc;i++){
            if(i>1)strcat(seed," ");
            strcat(seed,argv[i]);
        }
        printf("  Seed: %s\n\n  ",seed);
        think(b,seed);
        printf("\n");
        save(b);
        munmap(b,sizeof(Brain));
        return 0;
    }

    /* No args: interactive mode */
    printf("  Brain is alive. %lu neurons. %lu synapses.\n",
           (unsigned long)b->state.n_neurons,
           (unsigned long)b->state.n_synapses);
    printf("  Type something to teach it. Press enter with empty line to exit.\n\n");

    char input[4096];
    while(1){
        printf("> "); fflush(stdout);
        if(!fgets(input,4095,stdin)) break;
        char*p=input+strlen(input);
        while(p>input&&(*p=='\n'||*p=='\r'))*p--=0;
        if(strlen(input)==0) break;

        /* Learn from input */
        learn(b,input);

        /* Then think about it */
        printf("  "); think(b,input);
        printf("\n");

        /* Report brain state periodically */
        if(b->state.cycle%10==0){
            printf("  [Brain: %lu neurons, %lu synapses, %lu thoughts]\n\n",
                   (unsigned long)b->state.n_neurons,
                   (unsigned long)b->state.n_synapses,
                   (unsigned long)b->state.n_thoughts);
        }
    }

    save(b);
    printf("\n  Brain saved. %lu neurons, %lu synapses, %lu thoughts.\n",
           (unsigned long)b->state.n_neurons,
           (unsigned long)b->state.n_synapses,
           (unsigned long)b->state.n_thoughts);
    printf("  Goodbye.\n");
    munmap(b,sizeof(Brain));
    return 0;
}
