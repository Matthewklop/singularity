/* ============================================================================
 * live_bash.c — A shell that learns from every command you run
 *
 * Every command you type, it remembers the pattern.
 * The next time you need it, it completes from memory.
 * It's not a history file. It's a living knowledge base
 * that gets smarter the more you use it.
 *
 * Build: gcc -O3 -o live_bash live_bash.c -lm -lreadline
 * Run:   ./live_bash
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_PATTERNS 65536
#define MAX_LINE 4096

// ─── The living pattern table ───
static char patterns[MAX_PATTERNS][MAX_LINE];
static int pattern_count[MAX_PATTERNS];
static int n_patterns = 0;

// ─── Remember a command ───
static void remember(const char *cmd) {
    if (strlen(cmd) < 1) return;
    
    // Check if we've seen it before
    for (int i = 0; i < n_patterns; i++) {
        if (strcmp(patterns[i], cmd) == 0) {
            pattern_count[i]++;
            return;
        }
    }
    
    // New pattern
    if (n_patterns < MAX_PATTERNS) {
        strcpy(patterns[n_patterns], cmd);
        pattern_count[n_patterns] = 1;
        n_patterns++;
    }
}

// ─── Suggest a completion based on what we've learned ───
static const char *suggest(const char *partial) {
    if (strlen(partial) < 1 || n_patterns == 0) return NULL;
    
    int best_idx = -1;
    int best_count = 0;
    int best_pos = 0;
    
    for (int i = 0; i < n_patterns; i++) {
        const char *match = strstr(patterns[i], partial);
        if (match) {
            int pos = match - patterns[i];
            // Prefer: exact start > earlier match > more frequent
            int score = pattern_count[i] * 100;
            if (pos == 0) score *= 10;  // starts with = 10x
            score -= pos;               // earlier match = better
            if (score > best_count) {
                best_count = score;
                best_idx = i;
                best_pos = pos;
            }
        }
    }
    
    if (best_idx >= 0) return patterns[best_idx];
    return NULL;
}

// ─── Execute a command ───
static int execute(const char *cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child: execute the command
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        exit(1);
    } else if (pid > 0) {
        // Parent: wait for completion
        int status;
        waitpid(pid, &status, 0);
        return WEXITSTATUS(status);
    }
    return -1;
}

// ─── Save and load the pattern table ───
static void save_patterns(void) {
    FILE *f = fopen(".live_bash_patterns", "w");
    if (!f) return;
    fprintf(f, "%d\n", n_patterns);
    for (int i = 0; i < n_patterns; i++) {
        fprintf(f, "%d %s\n", pattern_count[i], patterns[i]);
    }
    fclose(f);
}

static void load_patterns(void) {
    FILE *f = fopen(".live_bash_patterns", "r");
    if (!f) return;
    fscanf(f, "%d\n", &n_patterns);
    for (int i = 0; i < n_patterns && i < MAX_PATTERNS; i++) {
        char buf[MAX_LINE];
        if (!fgets(buf, sizeof(buf), f)) break;
        // Parse: count + space + command
        char *space = strchr(buf, ' ');
        if (space) {
            *space = 0;
            pattern_count[i] = atoi(buf);
            strcpy(patterns[i], space + 1);
            // Remove trailing newline
            size_t len = strlen(patterns[i]);
            if (len > 0 && patterns[i][len-1] == '\n') patterns[i][len-1] = 0;
        }
    }
    fclose(f);
}

// ─── Signal handler for clean shutdown ───
static void handle_signal(int sig) {
    printf("\n  Saving patterns...\n");
    save_patterns();
    printf("  %d patterns remembered. Goodbye.\n", n_patterns);
    exit(0);
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    load_patterns();
    
    printf("╔══════════════════════════════════════╗\n");
    printf("║         LIVE BASH                    ║\n");
    printf("║  A shell that remembers everything   ║\n");
    printf("╚══════════════════════════════════════╝\n\n");
    printf("  Patterns loaded: %d\n", n_patterns);
    printf("  Type commands. It learns.\n");
    printf("  Tab completion from memory.\n");
    printf("  Ctrl-C to save and exit.\n\n");
    
    char cmd[MAX_LINE];
    
    while (1) {
        printf("  \033[1m$\033[0m ");
        fflush(stdout);
        
        if (!fgets(cmd, sizeof(cmd), stdin)) break;
        size_t len = strlen(cmd);
        if (len > 0 && cmd[len-1] == '\n') cmd[len-1] = '\0';
        
        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            break;
        }
        
        if (strlen(cmd) == 0) continue;
        
        // Remember the command
        remember(cmd);
        
        // Check for completion request
        if (cmd[len-1] == '?') {
            cmd[len-1] = '\0';
            const char *s = suggest(cmd);
            if (s) {
                printf("  -> %s\n", s);
            } else {
                printf("  (no pattern learned yet for that)\n");
            }
            continue;
        }
        
        // Execute the command
        execute(cmd);
    }
    
    save_patterns();
    printf("\n  %d patterns remembered. Goodbye.\n", n_patterns);
    
    return 0;
}
