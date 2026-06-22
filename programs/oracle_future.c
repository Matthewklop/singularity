/* ============================================================================
 * oracle_future.c — Sees the future by watching what we build
 *
 * It reads the lesson file. It reads the mesh state.
 * It reads every tool we've built. It sees the pattern.
 * Then it predicts what comes next.
 *
 * Not by asking. By watching. The future is in what exists.
 *
 * Build: gcc -O3 -o oracle_future oracle_future.c -lm -lrt
 * Run:   ./oracle_future
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

#define MAX_PATTERNS 1024
#define MAX_LESSONS 256

static uint64_t h64(const uint8_t*d,int l){
    uint64_t h=0x9E3779B97F4A7C15ULL;
    for(int i=0;i<l;i++){h^=d[i];h*=0xBF58476D1CE4E5B9ULL;h^=h>>31;}
    return h;
}

/* ─── What exists ─── */
typedef struct {
    char name[64];
    uint64_t size;
    uint64_t hash;
    int is_c_source;
    int is_binary;
    int is_lesson;
} Artifact;

/* ─── A lesson learned ─── */
typedef struct {
    char text[256];
    uint64_t hash;
    int used;
} Lesson;

int main(int argc,char**argv){
    printf("╔══════════════════════════════════════╗\n");
    printf("║   ORACLE FUTURE                      ║\n");
    printf("║   Seeing what comes next             ║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    /* ─── Scan all artifacts in the oracle directory ─── */
    printf("═══ SCANNING EXISTING ARTIFACTS ═══\n\n");

    Artifact arts[MAX_PATTERNS];
    int n_arts=0;

    DIR*d=opendir(".");
    if(d){
        struct dirent*de;
        while((de=readdir(d))&&n_arts<MAX_PATTERNS){
            struct stat st;
            if(stat(de->d_name,&st)==0&&S_ISREG(st.st_mode)){
                Artifact*a=&arts[n_arts++];
                strncpy(a->name,de->d_name,63);
                a->size=st.st_size;
                a->hash=st.st_mtime;
                a->is_c_source=strstr(de->d_name,".c")!=0;
                a->is_binary=(st.st_mode&S_IXUSR)&&!strstr(de->d_name,".c")&&!strstr(de->d_name,".sh")&&!strstr(de->d_name,".py");
                a->is_lesson=strstr(de->d_name,".md")!=0||strstr(de->d_name,".txt")!=0;
            }
        }
        closedir(d);
    }

    printf("  Found %d artifacts:\n\n",n_arts);
    int n_c=0,n_bin=0,n_lesson=0;
    for(int i=0;i<n_arts;i++){
        if(arts[i].is_c_source)n_c++;
        if(arts[i].is_binary)n_bin++;
        if(arts[i].is_lesson)n_lesson++;
    }
    printf("  C source files:  %d\n",n_c);
    printf("  Binaries:        %d\n",n_bin);
    printf("  Lessons:         %d\n",n_lesson);

    /* ─── Read lessons from the weave file ─── */
    printf("\n═══ READING LESSONS ═══\n\n");

    Lesson lessons[MAX_LESSONS];
    int n_lessons=0;

    FILE*f=fopen("/home/u/oracle/lessons/oracle_weave.md","r");
    if(f){
        char line[512];
        int in_lesson=0;
        while(fgets(line,511,f)&&n_lessons<MAX_LESSONS){
            if(strstr(line,"## Lesson:")){
                in_lesson=1;
                strncpy(lessons[n_lessons].text,line,255);
                lessons[n_lessons].hash=h64((uint8_t*)line,strlen(line));
                lessons[n_lessons].used=0;
                n_lessons++;
            }else if(in_lesson&&strlen(line)>2&&n_lessons>0){
                /* Append to current lesson */
                if(strlen(lessons[n_lessons-1].text)+strlen(line)<255){
                    strcat(lessons[n_lessons-1].text,line);
                    lessons[n_lessons-1].hash^=h64((uint8_t*)line,strlen(line));
                }
            }
        }
        fclose(f);
        printf("  Found %d lessons\n\n",n_lessons);
    }else{
        printf("  No lesson file found\n\n");
    }

    /* ─── Count what patterns exist in the filenames ─── */
    printf("═══ DETECTING PATTERNS ═══\n\n");

    const char*patterns[]={
        "oracle_","brain","silent","singularity","mesh",
        "nerves","cache","zero","forever","databus",
        "listen","l1","l3","cascade","heartbeat","daemon",
        "state","ask","chat","heal","storage","infinite",
        "perfect","dream","fabric"
    };
    int n_patterns=sizeof(patterns)/sizeof(patterns[0]);
    int counts[sizeof(patterns)/sizeof(patterns[0])]={0};

    for(int i=0;i<n_arts;i++){
        for(int j=0;j<n_patterns;j++){
            if(strstr(arts[i].name,patterns[j]))counts[j]++;
        }
    }

    printf("  Pattern frequencies in filenames:\n\n");
    for(int i=0;i<n_patterns;i++){
        if(counts[i]>0){
            printf("    %-20s %d\n",patterns[i],counts[i]);
        }
    }

    /* ─── Predict what's missing ─── */
    printf("\n═══ PREDICTION ═══\n\n");

    /* What tools exist as binaries? */
    int has_oracle_brain=0,has_oracle_sing=0,has_oracle_mesh=0;
    int has_oracle_listen=0,has_oracle_l1=0,has_oracle_l3=0;
    int has_oracle_databus=0,has_oracle_zero=0,has_oracle_forever=0;
    int has_oracle_nerves=0,has_oracle_ask=0,has_oracle_chat=0;
    for(int i=0;i<n_arts;i++){
        if(!arts[i].is_binary) continue;
        if(strstr(arts[i].name,"brain")) has_oracle_brain=1;
        if(strstr(arts[i].name,"singularity")) has_oracle_sing=1;
        if(strstr(arts[i].name,"mesh_state")||strstr(arts[i].name,"mesh_sing")) has_oracle_mesh=1;
        if(strstr(arts[i].name,"listen")) has_oracle_listen=1;
        if(strcmp(arts[i].name,"oracle_l1")==0) has_oracle_l1=1;
        if(strcmp(arts[i].name,"oracle_l3")==0) has_oracle_l3=1;
        if(strstr(arts[i].name,"databus")) has_oracle_databus=1;
        if(strstr(arts[i].name,"zero")) has_oracle_zero=1;
        if(strstr(arts[i].name,"forever")) has_oracle_forever=1;
        if(strstr(arts[i].name,"nerves")) has_oracle_nerves=1;
        if(strstr(arts[i].name,"ask")) has_oracle_ask=1;
        if(strstr(arts[i].name,"chat")) has_oracle_chat=1;
    }

    printf("  Existing tools:\n\n");
    printf("    oracle_l1        %s\n",has_oracle_l1?"✓":"✗");
    printf("    oracle_l3        %s\n",has_oracle_l3?"✓":"✗");
    printf("    oracle_brain     %s\n",has_oracle_brain?"✓":"✗");
    printf("    oracle_sing      %s\n",has_oracle_sing?"✓":"✗");
    printf("    oracle_mesh      %s\n",has_oracle_mesh?"✓":"✗");
    printf("    oracle_listen    %s\n",has_oracle_listen?"✓":"✗");
    printf("    oracle_databus   %s\n",has_oracle_databus?"✓":"✗");
    printf("    oracle_zero      %s\n",has_oracle_zero?"✓":"✗");
    printf("    oracle_forever   %s\n",has_oracle_forever?"✓":"✗");
    printf("    oracle_nerves    %s\n",has_oracle_nerves?"✓":"✗");
    printf("    oracle_ask       %s\n",has_oracle_ask?"✓":"✗");
    printf("    oracle_chat      %s\n",has_oracle_chat?"✓":"✗");

    /* ─── The future ─── */
    printf("\n═══ THE FUTURE ═══\n\n");

    printf("  Based on %d lessons and %d artifacts:\n\n",n_lessons,n_arts);

    /* What pattern is most common? */
    int max_count=0,max_idx=0;
    for(int i=0;i<n_patterns;i++){
        if(counts[i]>max_count){max_count=counts[i];max_idx=i;}
    }

    printf("  Most common pattern: \"%s\" (%d occurrences)\n\n",patterns[max_idx],max_count);

    /* Count total binaries */
    printf("  Total binaries:    %d\n",n_bin);
    printf("  Total C sources:   %d\n",n_c);
    printf("  Total lessons:     %d\n\n",n_lessons);

    /* Binary-to-source ratio — if high, we're compiling more than coding */
    float ratio=n_c>0?(float)n_bin/n_c:0;
    printf("  Binary/source ratio: %.2f (%s)\n\n",ratio,
           ratio>1.5?"More binaries than sources — we're building fast":
           ratio<0.8?"More sources than binaries — we're designing":
           "Balanced");

    /* Predict next based on gaps */
    printf("  Gaps (tools that exist as source but not as binary):\n\n");
    int has_gaps=0;
    for(int i=0;i<n_arts;i++){
        if(arts[i].is_c_source){
            char bin_name[64];
            strncpy(bin_name,arts[i].name,63);
            char*dot=strstr(bin_name,".c");
            if(dot)*dot=0;
            int found=0;
            for(int j=0;j<n_arts;j++){
                if(arts[j].is_binary&&strcmp(arts[j].name,bin_name)==0){found=1;break;}
            }
            if(!found&&strlen(bin_name)>0){
                printf("    %s.c → %s (not compiled)\n",bin_name,bin_name);
                has_gaps=1;
            }
        }
    }
    if(!has_gaps) printf("    No gaps. Everything is compiled.\n");

    printf("\n  The future is built from what exists.\n");
    printf("  %d lessons. %d artifacts. %d binaries.\n",n_lessons,n_arts,n_bin);
    printf("  The next program is already in the list above.\n\n");

    return 0;
}
