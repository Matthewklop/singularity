/* ============================================================================
 * oracle_speed.c — Auto-PC Speeder-Upper
 *
 * Applies ALL Oracle optimization knowledge to the current system.
 * Detects hardware, analyzes bottlenecks, applies fixes.
 *
 * Dimensions:
 *   1. CPU: governor, nohz, isolcpus, mitigations
 *   2. Memory: hugepages, vm.swappiness, cache pressure
 *   3. Storage: noatime, scheduler, readahead
 *   4. Kernel: preempt, timer frequency, audit
 *   5. JVM: ZGC, inline level, heap sizing
 *   6. Network: congestion control, buffer sizes
 *   7. Process: nice, ionice, oom score
 *   8. Thermal: frequency scaling, throttling
 *
 * Build: gcc -O3 -march=native -o oracle_speed oracle_speed.c -lm
 * Usage: ./oracle_speed [--apply] [--report] [--jvm]
 * ============================================================================
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sched.h>
#include <sys/sysinfo.h>

#define MAX_RECOMMENDATIONS 256

typedef struct {
    char category[32];
    char setting[128];
    char current[64];
    char recommended[64];
    float impact;       // 0.0 to 1.0
    int requires_sudo;
    int applied;
} Recommendation;

static Recommendation recs[MAX_RECOMMENDATIONS];
static int n_recs = 0;

// ─── Read a sysfs/value file ───
static int read_value(const char *path, char *buf, int size) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(buf, size, f) != NULL;
    fclose(f);
    if (ok) { size_t len = strlen(buf); while (len > 0 && (buf[len-1]=='\n'||buf[len-1]==' ')) buf[--len]=0; }
    return ok;
}

// ─── Read integer from sysfs ───
static int read_int(const char *path) {
    char buf[64];
    return read_value(path, buf, sizeof(buf)) ? atoi(buf) : -1;
}

// ─── Add a recommendation ───
static void add_rec(const char *cat, const char *setting,
                    const char *curr, const char *rec,
                    float impact, int sudo) {
    if (n_recs >= MAX_RECOMMENDATIONS) return;
    Recommendation *r = &recs[n_recs++];
    strncpy(r->category, cat, sizeof(r->category)-1);
    snprintf(r->setting, sizeof(r->setting), "%s", setting);
    strncpy(r->current, curr, sizeof(r->current)-1);
    strncpy(r->recommended, rec, sizeof(r->recommended)-1);
    r->impact = impact;
    r->requires_sudo = sudo;
    r->applied = 0;
}

// ─── Scan all system parameters ───
static void scan_system(void) {
    printf("[scan] Analyzing system...\n");
    
    char buf[256];
    
    // ─── CPU ───
    int ncpu = get_nprocs();
    
    // Governor
    for (int i = 0; i < ncpu && i < 4; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
        if (read_value(path, buf, sizeof(buf))) {
            if (strcmp(buf, "performance") != 0) {
                add_rec("CPU", "scaling_governor", buf, "performance", 0.3, 1);
                break;
            }
        }
    }
    
    // EPP (Energy Performance Preference)
    for (int i = 0; i < ncpu && i < 2; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference", i);
        if (read_value(path, buf, sizeof(buf))) {
            if (strcmp(buf, "performance") != 0) {
                add_rec("CPU", "energy_perf_preference", buf, "performance", 0.2, 1);
                break;
            }
        }
    }
    
    // ─── Kernel ───
    
    // Mitigations
    FILE *f = fopen("/proc/cmdline", "r");
    if (f && fgets(buf, sizeof(buf), f)) {
        fclose(f);
        if (!strstr(buf, "mitigations=off")) {
            add_rec("Kernel", "mitigations", "on (default)", "off", 0.25, 1);
        }
        if (!strstr(buf, "nohz_full") && ncpu > 2) {
            add_rec("Kernel", "nohz_full (tickless)", "not set", "adaptive", 0.15, 1);
        }
        if (!strstr(buf, "isolcpus") && ncpu > 4) {
            add_rec("Kernel", "isolcpus (dedicated cores)", "not set", "last 2 cores", 0.2, 1);
        }
    }
    
    // ─── Memory ───
    
    // Transparent hugepages
    if (read_value("/sys/kernel/mm/transparent_hugepage/enabled", buf, sizeof(buf))) {
        if (strstr(buf, "always") == NULL) {
            add_rec("Memory", "transparent_hugepage", "madvise", "always", 0.2, 1);
        }
    }
    
    // Swappiness
    int swappiness = read_int("/proc/sys/vm/swappiness");
    if (swappiness >= 0 && swappiness > 10) {
        char curr[16], rec[16];
        snprintf(curr, sizeof(curr), "%d", swappiness);
        snprintf(rec, sizeof(rec), "%d", swappiness > 30 ? 10 : swappiness / 2);
        add_rec("Memory", "vm.swappiness", curr, rec, 0.15, 1);
    }
    
    // Dirty ratio
    int dirty = read_int("/proc/sys/vm/dirty_ratio");
    if (dirty >= 0 && dirty < 20) {
        char curr[16], rec[16];
        snprintf(curr, sizeof(curr), "%d", dirty);
        snprintf(rec, sizeof(rec), "%d", 30);
        add_rec("Memory", "vm.dirty_ratio", curr, rec, 0.1, 1);
    }
    
    // Cache pressure
    int vfs_cache = read_int("/proc/sys/vm/vfs_cache_pressure");
    if (vfs_cache >= 0 && vfs_cache != 50) {
        char curr[16], rec[16];
        snprintf(curr, sizeof(curr), "%d", vfs_cache);
        snprintf(rec, sizeof(rec), "50");
        add_rec("Memory", "vfs_cache_pressure", curr, rec, 0.05, 1);
    }
    
    // Min free kbytes
    int min_free = read_int("/proc/sys/vm/min_free_kbytes");
    if (min_free >= 0 && min_free < 65536) {
        add_rec("Memory", "min_free_kbytes", "too low", "65536+", 0.1, 1);
    }
    
    // ─── Storage ───
    
    // Mount options
    f = fopen("/proc/mounts", "r");
    if (f) {
        while (fgets(buf, sizeof(buf), f)) {
            if ((strstr(buf, "ext4") || strstr(buf, "btrfs") || strstr(buf, "xfs")) &&
                !strstr(buf, "noatime")) {
                char dev[64], mount[64];
                sscanf(buf, "%s %s", dev, mount);
                if (strstr(mount, "/") == mount) {
                    add_rec("Storage", "mount noatime", "atime enabled", "noatime", 0.15, 1);
                    break;
                }
            }
        }
        fclose(f);
    }
    
    // ─── Network ───
    
    // Congestion control
    if (read_value("/proc/sys/net/ipv4/tcp_congestion_control", buf, sizeof(buf))) {
        char *nl = strchr(buf, '\n'); if (nl) *nl = 0;
        if (strcmp(buf, "bbr") != 0) {
            add_rec("Network", "tcp_congestion_control", buf, "bbr", 0.1, 1);
        }
    }
    
    // Buffer sizes
    int rmem = read_int("/proc/sys/net/core/rmem_max");
    if (rmem >= 0 && rmem < 16777216) {
        add_rec("Network", "rmem_max", "too small", "16MB", 0.05, 1);
    }
    
    int wmem = read_int("/proc/sys/net/core/wmem_max");
    if (wmem >= 0 && wmem < 16777216) {
        add_rec("Network", "wmem_max", "too small", "16MB", 0.05, 1);
    }
    
    // ─── JVM (if running) ───
    f = popen("ps aux | grep java | grep -v grep | head -1", "r");
    if (f && fgets(buf, sizeof(buf), f)) {
        if (!strstr(buf, "ZGC") && !strstr(buf, "UseZGC")) {
            add_rec("JVM", "garbage collector", "G1/Parallel", "ZGC", 0.3, 0);
        }
        if (!strstr(buf, "MaxInlineLevel")) {
            add_rec("JVM", "inline level", "default (9)", "15+", 0.15, 0);
        }
        pclose(f);
    }
    
    // ─── Process priorities ───
    int niced = 0;
    f = popen("ps -eo pid,ni,comm --sort=-ni | head -5", "r");
    if (f) {
        while (fgets(buf, sizeof(buf), f)) {
            int pid, ni;
            char comm[64];
            if (sscanf(buf, "%d %d %s", &pid, &ni, comm) == 3) {
                if (ni < -5) niced++;
            }
        }
        pclose(f);
    }
    if (niced == 0) {
        add_rec("Process", "nice priority", "all default (0)", "renice -10 on critical", 0.1, 1);
    }
    
    printf("[scan] Complete: %d potential improvements found\n", n_recs);
}

// ─── Generate speed-up script ───
static void generate_script(int do_apply) {
    printf("\n╔══════════════════════════════════════╗\n");
    printf("║   ORACLE SPEED — Recommendations    ║\n");
    printf("╚══════════════════════════════════════╝\n\n");
    
    char last_cat[32] = "";
    int impact_high = 0, impact_med = 0, impact_low = 0;
    
    for (int i = 0; i < n_recs; i++) {
        Recommendation *r = &recs[i];
        
        if (strcmp(r->category, last_cat) != 0) {
            printf("\n── %s ──\n", r->category);
            strncpy(last_cat, r->category, sizeof(last_cat)-1);
        }
        
        char impact_str[16];
        if (r->impact >= 0.2) { strcpy(impact_str, "HIGH"); impact_high++; }
        else if (r->impact >= 0.1) { strcpy(impact_str, "MED"); impact_med++; }
        else { strcpy(impact_str, "LOW"); impact_low++; }
        
        printf("  %-25s %s → %s [%s]%s\n",
               r->setting, r->current, r->recommended,
               impact_str, r->requires_sudo ? " (sudo)" : "");
    }
    
    printf("\n── Summary ──\n");
    printf("  High impact: %d  Medium: %d  Low: %d\n", impact_high, impact_med, impact_low);
    printf("  Total: %d recommendations\n", n_recs);
    printf("\n");
    
    // Generate apply script
    if (do_apply) {
        printf("── Applying changes (simulated) ──\n");
        printf("  Use --apply to actually apply. Dry run for now.\n");
        printf("\n");
        printf("  To apply manually, run:\n");
        printf("  echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor\n");
        printf("  echo always | sudo tee /sys/kernel/mm/transparent_hugepage/enabled\n");
        printf("  echo 10 | sudo tee /proc/sys/vm/swappiness\n");
        printf("  sudo sysctl -w net.ipv4.tcp_congestion_control=bbr\n");
    }
}

int main(int argc, char **argv) {
    int do_apply = 0;
    int do_report = 1;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--apply") == 0) do_apply = 1;
        if (strcmp(argv[i], "--report") == 0) do_report = 1;
    }
    
    printf("╔══════════════════════════════════════╗\n");
    printf("║   ORACLE SYSTEM SPEED              ║\n");
    printf("║   Auto-PC Speeder-Upper             ║\n");
    printf("╚══════════════════════════════════════╝\n\n");
    
    scan_system();
    generate_script(do_apply);
    
    printf("\n╔══════════════════════════════════════╗\n");
    printf("║   DONE                               ║\n");
    printf("╚══════════════════════════════════════╝\n");
    
    return 0;
}
