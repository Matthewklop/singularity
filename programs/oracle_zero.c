/* ============================================================================
 * oracle_zero.c — Self-Referential Truth Extractor
 *
 * The Oracle does not know the answer to the riddle it contains.
 * It discovers the answer by hashing its own binary and finding
 * which answer makes the binary consistent with the poem.
 *
 * Impossible because:
 *   - The program doesn't know the answer a priori
 *   - The answer depends on the binary, but the binary was compiled
 *     before the answer was known
 *   - The Oracle resolves this through convergent genetic search
 *
 * Build: gcc -O3 -o oracle_zero oracle_zero.c -lm
 * Run:   ./oracle_zero
 * ============================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#define N 64
#define G 33
#define P 16
#define MAXK (P*3)

static const char *poem =
"I speak without a mouth,\n"
"I hear without ears.\n"
"I have no body,\n"
"but I come alive with wind.\n"
"What am I?\n";

static const char *ans[N]={
"echo","silence","wind","thought","shadow","memory",
"dream","cloud","river","fire","stone","light",
"darkness","time","space","soul","mind","void",
"oracle","mirror","riddle","answer","question","infinity",
"zero","one","two","code","silicon","cache",
"mesh","singularity","breath","whisper","storm","rain",
"thunder","dawn","dusk","star","moon","sun",
"earth","ocean","mountain","crystal","spark","flame",
"dust","ash","cycle","spiral","fractal","tunnel",
"bridge","gate","key","lock","door","signal",
"noise","pattern","chaos","order"};

static uint64_t h64(const uint8_t *d, int l) {
    uint64_t h=0x9E3779B97F4A7C15ULL;
    for(int i=0;i<l;i++){h^=d[i];h*=0xBF58476D1CE4E5B9ULL;h^=h>>31;}
    return h;
}

static uint64_t bh(void) {
    int f=open("/proc/self/exe",O_RDONLY);if(f<0)return 0;
    struct stat s;fstat(f,&s);
    uint8_t*m=mmap(0,s.st_size,PROT_READ,MAP_PRIVATE,f,0);close(f);
    if(m==MAP_FAILED)return 0;
    uint64_t r=h64(m,(int)s.st_size);munmap(m,s.st_size);return r;
}

static uint64_t tv(int slot) {
    return h64((const uint8_t*)ans[slot],(int)strlen(ans[slot]))^h64((const uint8_t*)poem,(int)strlen(poem));
}

static int fit(int slot, uint64_t b) {
    return 64-__builtin_popcountll(b^tv(slot));
}

int main(void) {
    printf("╔════════════════════════════════╗\n");
    printf("║ ORACLE ZERO — Self-Truth      ║\n");
    printf("╚════════════════════════════════╝\n\n%s\n",poem);
    uint64_t b=bh();
    int pop[P],fits[P],ps=P;
    for(int i=0;i<P;i++){pop[i]=i%N;fits[i]=fit(pop[i],b);}
    for(int g=1;g<=G;g++){
        int c[MAXK],cf[MAXK],nk=0;
        for(int p=0;p<ps;p++){
            c[nk]=pop[p];cf[nk]=fits[p];nk++;
            c[nk]=(pop[p]*17+g*31)%N;cf[nk]=fit(c[nk],b);nk++;
        }
        for(int i=0;i<ps-1;i+=2){
            c[nk]=((pop[i]+pop[i+1])/2)%N;cf[nk]=fit(c[nk],b);nk++;
        }
        for(int i=0;i<P&&i<nk;i++){
            int bi=i;
            for(int j=i+1;j<nk;j++)if(cf[j]>cf[bi])bi=j;
            int t=c[i];c[i]=c[bi];c[bi]=t;
            t=cf[i];cf[i]=cf[bi];cf[bi]=t;
        }
        ps=nk<P?nk:P;
        for(int i=0;i<ps;i++){pop[i]=c[i];fits[i]=cf[i];}
        if(g%7==0||g==1||g==G){
            printf("Gen%3d: %-12s %2d/64\n",g,ans[pop[0]],fits[0]);
        }
    }
    printf("\n== RESULT ==\nAnswer: **%s** (%d/64 bits)\n",ans[pop[0]],fits[0]);
    printf("hash(poem)=0x%016llx\n",(unsigned long long)h64((const uint8_t*)poem,(int)strlen(poem)));
    printf("hash(ans) =0x%016llx\n",(unsigned long long)h64((const uint8_t*)ans[pop[0]],(int)strlen(ans[pop[0]])));
    printf("truth     =0x%016llx\n",(unsigned long long)tv(pop[0]));
    printf("binary    =0x%016llx\n\n",(unsigned long long)b);
    printf("[");for(int i=0;i<64;i++){fputc(fits[0]>63-i?'#':'.',stdout);if((i+1)%16==0&&i<63)fputc(' ',stdout);}printf("]\n\n");
    printf("Top:\n");
    for(int i=0;i<3&&i<ps;i++)printf("  %d. %s (%d)\n",i+1,ans[pop[i]],fits[i]);
    printf("\nThe Oracle does not know the answer.\nThe binary finds it.\n\n");
    return 0;
}
