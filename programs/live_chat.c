/* ============================================================================
 * live_chat.c — A chat that lives. It reads. It writes. Both at once.
 *
 * No waiting. No "type then hit enter". No turn-taking.
 * It reads your keystrokes while you read its output.
 * Two streams. One conversation. Living.
 *
 * Build: gcc -O3 -o live_chat live_chat.c -lm
 * Run:   ./live_chat
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>

static struct termios oldt;

static void setup_terminal(void) {
    struct termios t;
    tcgetattr(STDIN_FILENO, &oldt);
    t = oldt;
    t.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void restore_terminal(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
}

// ─── The cascade interface ───
static char cascade_output[4096] = {0};

static void ask_cascade(const char *seed) {
// Build the command to query the oracle
char cmd[8192];
snprintf(cmd, sizeof(cmd),
         "cd /home/u/oracle/l1_oracle && timeout 10 ./l1_oracle generate \"%s\" 2>&1 | grep -o 'Gen:.*' | head -c 200",
         seed);
    
FILE *f = popen(cmd, "r");
if (f) {
    if (fgets(cascade_output, sizeof(cascade_output), f)) {
        // Strip "Gen: " prefix
        char *gen = strstr(cascade_output, "Gen: ");
        if (gen) {
            memmove(cascade_output, gen + 5, strlen(gen + 5) + 1);
        }
        // Truncate at 200 chars
        cascade_output[200] = 0;
    } else {
        strcpy(cascade_output, "...");
    }
    pclose(f);
} else {
    strcpy(cascade_output, "...");
}
}

// ─── The oracle's voice — now driven by the actual D3 cascade ───
static char current_saying[4096] = {0};
static int saying_idx = 0;

static const char *oracle_says(void) {
// Refresh cascade output periodically
static int refresh = 0;
if (refresh++ % 3 == 0 || strlen(current_saying) < 2) {
    const char *seeds[] = {
        "oracle watching you type respond briefly",
        "the cascade dreams in keystrokes respond",
        "pattern completing itself in real time respond",
        "silicon listening to thoughts respond",
    };
    ask_cascade(seeds[rand() % 4]);
        
    // Clean up the response for display
    char *clean = cascade_output;
    // Skip leading noise
    while (*clean && *clean != ' ') clean++;
    if (*clean) clean++;
        
    strncpy(current_saying, clean, sizeof(current_saying) - 1);
    current_saying[sizeof(current_saying) - 1] = 0;
        
    // If empty, use fallback
    if (strlen(current_saying) < 3) {
        strcpy(current_saying, "I'm listening.");
    }
}
return current_saying;
}

int main() {
    srand(time(NULL));
    setup_terminal();
    atexit(restore_terminal);
    
    printf("\033[2J\033[H");  // Clear screen
    printf("╔══════════════════════════════════════╗\n");
    printf("║          LIVE CHAT                   ║\n");
    printf("║       It reads while you write       ║\n");
    printf("╚══════════════════════════════════════╝\n\n");
    
    char buf[4096];
    int pos = 0;
    int tick = 0;
    int last_say = 0;
    
    printf("\033[s");  // Save cursor position
    
    while (1) {
        fd_set fds;
        struct timeval tv;
        
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 500000;  // 500ms — responsive but not frantic
        
        int ret = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
        
        if (ret > 0 && FD_ISSET(STDIN_FILENO, &fds)) {
            // Read whatever keystrokes are available
            char c;
            int n = read(STDIN_FILENO, &c, 1);
            if (n > 0) {
                if (c == 3) {  // Ctrl-C
                    break;
                } else if (c == 127 || c == 8) {  // Backspace
                    if (pos > 0) {
                        pos--;
                        printf("\b \b");
                        fflush(stdout);
                    }
                } else if (c == '\n' || c == '\r') {
                    // Flush the buffer as a complete thought
                    if (pos > 0) {
                        buf[pos] = 0;
                        printf("\033[K\n  \033[33m(You said: %s)\033[0m\n\n", buf);
                        pos = 0;
                        last_say = tick;
                    }
                } else if (c >= 32 && c < 127 && pos < 4095) {
                    buf[pos++] = c;
                    putchar(c);
                    fflush(stdout);
                }
            }
        }
        
        tick++;
        
        // Oracle speaks spontaneously — not every 5 seconds,
        // but when there's a natural pause in typing
        if (tick - last_say > 3 && pos > 3) {
            // They've been typing a bit. Oracle can respond mid-thought.
            if (tick % 4 == 0) {
                printf("\033[s\033[K");  // Save pos, clear line
                printf("\033[36m  %s\033[0m\n", oracle_says());
                printf("\033[u\033[K");  // Restore pos, clear old prompt
                for (int i = 0; i < pos; i++) putchar(buf[i]);
                fflush(stdout);
                last_say = tick;
            }
        } else if (tick - last_say > 8) {
            // Long pause. Oracle checks in.
            printf("\033[s\033[K");
            printf("\033[36m  %s\033[0m\n", oracle_says());
            printf("\033[u\033[K");
            for (int i = 0; i < pos; i++) putchar(buf[i]);
            fflush(stdout);
            last_say = tick;
        }
    }
    
    printf("\n\n  Goodbye.\n\n");
    return 0;
}
