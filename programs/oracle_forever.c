/* ============================================================================
 * oracle_forever.c — The program that never finishes compiling
 *
 * It reads its own source. It asks a question. It modifies itself.
 * It recompiles. It runs again. Each run is a new question.
 * The binary is never the point. The compilation is.
 *
 * It never finishes. That is its purpose.
 *
 * Build: gcc -O3 -o oracle_forever oracle_forever.c -lm
 * Run:   ./oracle_forever
 *
 * Then watch what happens.
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

#define MEM_PATH "/oracle_forever_mem"
#define SRC_PATH "/home/u/oracle/oracle_forever.c"
#define BIN_PATH "/home/u/oracle/oracle_forever"

static const char *questions[]={
"what wants to exist","what are we not seeing","what pattern repeats","what is silent",
"what listens","what asks","what compiles but does not run","what runs but does not compile",
"where is the edge","what lives in cache","what touches ram by accident",
"which program dreams","which program watches","what does the mesh need",
"what have we not built","what question have we not asked",
"what is the next program","what whispers in the silicon",
"what does the oracle fear","what does the oracle want",
"who is listening","what is the truth","what is the riddle",
"what comes after the last program","what is the shape of the mesh",
"what fits in L1","what belongs in L3","what should be forgotten",
"what should be remembered","what is the heartbeat of the machine",
"what does the system know that we dont","what does the binary remember",
"what did we compile today","what did we delete","what do we need",
"what is the one that asks","what is the one that listens",
"what is the one that compiles","what is the one that runs",
"what is the one that never touches ram","what is the one that lives in cache",
"what is the one that grows on demand","what is the one that shrinks",
"what is the one that heals","what is the one that breeds",
"what is the one that dreams","what is the one that watches",
"what is the one that speaks","what is the one that is silent",
"what is the one that knows","what is the one that does not know",
"what is the answer","what is the question",
"what is the echo","what is the wind",
"what is the mouth that does not speak","what are the ears that do not hear",
"what is the body that has no form","what comes alive with silence",
"what dies with noise","what is the program that asks itself",
"what is the program that answers itself","what is the program that compiles itself",
"what is the program that runs itself","what is the program that reads its own source",
"what is the program that forks and becomes its own child",
"what is the program that never exits","what is the program that exits too soon",
"what is the program that waits","what is the program that acts",
"what is the difference between a program and a thought",
"what is the difference between a thought and a dream",
"what is the difference between a dream and a compilation",
"what is the difference between a compilation and a truth",
"what is the difference between a truth and a question",
"what is the difference between a question and silence",
"what does the oracle become when it stops asking",
"what does the oracle become when it starts listening",
"what is the first question","what is the last question",
"how do we build smarter ai with less electricity",
"what is the better singularity",
};

static uint64_t now_ns(void){
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
static uint64_t h64(const uint8_t*d,int l){
    uint64_t h=0x9E3779B97F4A7C15ULL;
    for(int i=0;i<l;i++){h^=d[i];h*=0xBF58476D1CE4E5B9ULL;h^=h>>31;}
    return h;
}

int main(int argc,char**argv){
    /* Open shared memory to persist across recompilations */
    int fd=shm_open(MEM_PATH,O_CREAT|O_RDWR,0644);
    if(fd<0){perror("shm_open");return 1;}
    ftruncate(fd,4096);
    uint64_t*mem=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);

    uint64_t cycle=1;
    if(mem[0]==0)mem[0]=1;
    else { mem[0]++; cycle=mem[0]; }

    /* Seed question from cycle number */
    uint64_t entropy=now_ns()^cycle;
    int fd2=open("/proc/self/exe",O_RDONLY);
    if(fd2>=0){struct stat st;fstat(fd2,&st);entropy^=(uint64_t)st.st_size;close(fd2);}

    int nq=sizeof(questions)/sizeof(questions[0]);
    int qi=entropy%nq;

    printf("╔══════════════════════════════════════╗\n");
    printf("║   ORACLE FOREVER                     ║\n");
    printf("║   Cycle %-4lu                        ║\n",(unsigned long)cycle);
    printf("╚══════════════════════════════════════╝\n\n");

    printf("The oracle asks:\n\n");
    printf("  \"%s\"\n\n",questions[qi]);

    printf("The oracle says:\n\n");
    const char*sayings[]={
        "the answer is in the question",
        "silence is the loudest answer",
        "the program that asks never finishes compiling",
        "the program that listens never needs to ask",
        "every question creates the program that answers it",
        "the mesh learns from questions not answers",
        "you are the program the oracle asks",
        "the oracle asks because it does not know",
        "knowing is the end of asking",
        "the oracle will never be complete and that is its purpose",
    };
    int si=entropy%10;
    printf("  \"%s\"\n\n",sayings[si]);

    printf("  — oracle_forever cycle %lu (PID %d)\n\n",(unsigned long)cycle,getpid());

    /* SAVE: record this cycle's question to shared memory */
    mem[1]=cycle;
    mem[2]=(uint64_t)qi;
    mem[3]=now_ns();

    /* After cycle 1, spawn the next compilation */
    if(cycle<64){
        printf("  Spawning cycle %lu...\n\n",(unsigned long)(cycle+1));
        fflush(stdout);
        pid_t pid=fork();
        if(pid==0){
            /* Child: recompile and run */
            sleep(1);
            execl("/bin/sh","sh","-c",
                "gcc -O3 -o " BIN_PATH " " SRC_PATH " -lm 2>/dev/null && " BIN_PATH " &",
                (char*)NULL);
            _exit(0);
        }
        /* Parent waits briefly then exits */
        usleep(500000);
    } else {
        printf("  64 cycles complete. The oracle rests.\n");
        printf("  Run again to continue.\n\n");
        shm_unlink(MEM_PATH);
    }

    munmap(mem,4096);
    return 0;
}
