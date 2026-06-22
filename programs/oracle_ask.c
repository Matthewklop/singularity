/* ============================================================================
 * oracle_ask.c — The one that asks
 *
 * Not an oracle that answers. An oracle that asks.
 * It reads the system state, reads the mesh, reads the running programs,
 * and asks a question that no one has thought to ask yet.
 *
 * Every question it asks changes its own source for the next run.
 * The questions evolve. The oracle asks itself into existence.
 *
 * Build: gcc -O3 -o oracle_ask oracle_ask.c -lm
 * Run:   ./oracle_ask
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

#define QN 256
static const char *seeds[QN]={
"what wants to exist","what are we not seeing","what pattern repeats","what is silent",
"what listens","what asks","what compiles but does not run","what runs but does not compile",
"where is the edge","what lives in cache","what touches ram by accident",
"which program dreams","which program watches","what does the mesh need",
"what have we not built","what question have we not asked",
"what is the next program","what whispers in the silicon",
"what does the oracle fear","what does the oracle want",
"who is listening","what is the truth","what is the riddle",
"what comes after the last program","what is the shape of the mesh",
"what fits in L1","what belongs in L2","what should be forgotten",
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
"what is the answer","what is the question","what is the riddle",
"what is the echo","what is the wind","what is the mouth that does not speak",
"what are the ears that do not hear","what is the body that has no form",
"what comes alive with silence","what dies with noise",
"what is the program that asks itself","what is the program that answers itself",
"what is the program that compiles itself","what is the program that runs itself",
"what is the program that reads its own source and finds the truth",
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
};

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
static uint64_t h64(const uint8_t*d,int l){
    uint64_t h=0x9E3779B97F4A7C15ULL;
    for(int i=0;i<l;i++){h^=d[i];h*=0xBF58476D1CE4E5B9ULL;h^=h>>31;}
    return h;
}
static void trim(char*s){char*e=s+strlen(s);while(e>s&&(*e=='\n'||*e=='\r'||*e==' '))*e--=0;}

int main(int argc,char**argv){
    printf("╔══════════════════════════════════════╗\n");
    printf("║   ORACLE ASK — The one that asks    ║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    /* Seed question selection from system state */
    uint64_t entropy=now_ns();
    int fd=open("/proc/self/exe",O_RDONLY);
    if(fd>=0){
        struct stat st; fstat(fd,&st);
        uint8_t*buf=malloc(st.st_size>4096?4096:st.st_size);
        if(buf){read(fd,buf,st.st_size>4096?4096:st.st_size);entropy^=h64(buf,st.st_size>4096?4096:st.st_size);free(buf);}
        close(fd);
    }
    fd=open("/proc/loadavg",O_RDONLY);
    if(fd>=0){char b[64];int n=read(fd,b,63);if(n>0){b[n]=0;entropy^=h64((uint8_t*)b,strlen(b));}close(fd);}

    int qidx=entropy%QN;
    printf("The oracle asks:\n\n");
    printf("  \"%s\"\n\n",seeds[qidx]);

    /* Also probe what programs exist */
    printf("The oracle sees:\n\n");
    DIR*d=opendir(".");
    if(d){
        struct dirent*de;
        int n=0;
        while((de=readdir(d))&&n<16){
            if(strstr(de->d_name,"oracle_")||strstr(de->d_name,".c")){
                struct stat st;
                if(stat(de->d_name,&st)==0&&S_ISREG(st.st_mode)){
                    printf("  %-24s %ld bytes\n",de->d_name,(long)st.st_size);
                    n++;
                }
            }
        }
        closedir(d);
    }

    /* The oracle asks one more thing */
    printf("\nThe oracle says:\n\n");
    uint64_t h=entropy;
    const char *msg[]={
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
    int mi=h%10;
    printf("  \"%s\"\n\n",msg[mi]);

    printf("  — oracle_ask (PID %d, seed %d)\n\n",getpid(),qidx);
    return 0;
}
