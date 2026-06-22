/* ============================================================================
 * post_mosfet.c — Storage and logic in one cell
 *
 * No fetch. No decode. No execute.
 * Every cell holds state AND transitions to the next state.
 * The program IS the data. The data IS the computation.
 *
 * This is what comes after the transistor.
 *
 * Build: gcc -O3 -o post_mosfet post_mosfet.c -lm
 * Run:   ./post_mosfet
 * ============================================================================
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#define N_CELLS 65536
#define N_STATES 256

// ─── A single cell: stores state, transitions to next state ───
typedef struct {
    uint8_t state;        // current value (0-255)
    uint8_t next[N_STATES];  // transition table: given neighbor states, what to become
    uint32_t hits;         // how many times this cell was read
    uint32_t writes;       // how many times this cell was written
} cell_t;

// ─── The fabric: all cells, all connections, all computation ───
typedef struct {
    cell_t cells[N_CELLS];
    uint8_t *input;        // external input stream
    uint32_t input_len;
    uint8_t *output;       // external output stream
    uint32_t output_len;
    uint32_t output_pos;
    uint32_t clock;        // global tick
    
    // Metrics
    uint64_t total_transitions;
    uint64_t total_energy_pj;  // femtojoules per transition
} fabric_t;

static fabric_t *create_fabric(uint32_t input_len, uint32_t output_len) {
    fabric_t *f = calloc(1, sizeof(fabric_t));
    f->input_len = input_len;
    f->output_len = output_len;
    f->input = calloc(input_len + 1, 1);
    f->output = calloc(output_len + 1, 1);
    f->output_pos = 0;
    f->clock = 0;
    f->total_transitions = 0;
    f->total_energy_pj = 0;
    
    // Initialize each cell with a random transition table
    srand(time(NULL));
    for (uint32_t i = 0; i < N_CELLS; i++) {
        f->cells[i].state = rand() & 0xFF;
        for (int s = 0; s < N_STATES; s++) {
            // Each cell has its own "personality" — a function from
            // current state to next state. This IS the program.
            f->cells[i].next[s] = (uint8_t)((s * i + (i >> 4) + (s << 3)) & 0xFF);
        }
    }
    
    return f;
}

// ─── Load input into the fabric ───
static void load_input(fabric_t *f, const uint8_t *data, uint32_t len) {
    uint32_t n = len < f->input_len ? len : f->input_len;
    memcpy(f->input, data, n);
    // Write input into the first N cells
    for (uint32_t i = 0; i < n && i < N_CELLS; i++) {
        f->cells[i].state = data[i];
        f->cells[i].writes++;
        f->total_energy_pj += 1;  // 1 fJ per write
    }
}

// ─── One clock tick: every cell transitions based on its neighbors ───
static void tick(fabric_t *f) {
    uint8_t old_states[N_CELLS];
    memcpy(old_states, f->cells, N_CELLS);
    
    // Each cell looks at its neighbors and its own state,
    // then transitions according to its transition table.
    // The transition function IS the computation.
    for (uint32_t i = 0; i < N_CELLS; i++) {
        // Gather neighbor states (up to 8 neighbors)
        uint32_t neighbor_hash = 0;
        if (i > 0) neighbor_hash ^= old_states[i-1];
        if (i < N_CELLS - 1) neighbor_hash ^= old_states[i+1];
        if (i > 100) neighbor_hash ^= old_states[i-100];
        if (i < N_CELLS - 100) neighbor_hash ^= old_states[i+100];
        neighbor_hash ^= old_states[i];  // include self
        
        uint8_t transition_key = (uint8_t)(neighbor_hash ^ (i & 0xFF));
        
        // Transition based on the cell's own transition table
        uint8_t new_state = f->cells[i].next[transition_key];
        
        // Mix in external input if we're an input cell
        if (i < f->input_len && f->input[i]) {
            new_state ^= f->input[i];
        }
        
        f->cells[i].state = new_state;
        f->cells[i].hits++;
        f->total_transitions++;
        f->total_energy_pj += 1;  // 1 fJ per transition
    }
    
    // Read output from the last N output cells
    f->output_pos = 0;
    for (uint32_t i = N_CELLS - f->output_len; i < N_CELLS && f->output_pos < f->output_len; i++) {
        f->output[f->output_pos++] = f->cells[i].state;
    }
    
    f->clock++;
}

// ─── Run the fabric for N ticks ───
static void run(fabric_t *f, uint32_t ticks) {
    printf("  Running for %u ticks...\n", ticks);
    for (uint32_t t = 0; t < ticks; t++) {
        tick(f);
        if (t % 10000 == 0 && t > 0) {
            printf("    tick %u: energy=%.3f nJ\n", t, f->total_energy_pj / 1000.0);
        }
    }
}

// ─── Read output from the fabric ───
static void read_output(fabric_t *f, uint8_t *out, uint32_t max_len) {
    uint32_t n = f->output_pos < max_len ? f->output_pos : max_len;
    memcpy(out, f->output, n);
}

// ─── Program the fabric: set a cell's transition table ───
static void program_cell(fabric_t *f, uint32_t addr, uint8_t state, uint8_t *transitions) {
    if (addr >= N_CELLS) return;
    f->cells[addr].state = state;
    if (transitions) {
        memcpy(f->cells[addr].next, transitions, N_STATES);
    }
    f->cells[addr].writes++;
    f->total_energy_pj += 256;  // 256 fJ to program a full transition table
}

// ─── Program the fabric: load a program into the transition tables ───
// The "program" is just data that determines how cells transition.
// There is no instruction set. The transition tables ARE the code.
static void load_program(fabric_t *f, const uint8_t *program, uint32_t len) {
    for (uint32_t i = 0; i < len && i < N_CELLS; i++) {
        uint8_t transitions[N_STATES];
        for (int s = 0; s < N_STATES; s++) {
            // The program byte determines the transition function
            transitions[s] = (uint8_t)((s * program[i] + i) & 0xFF);
        }
        program_cell(f, i, program[i % len], transitions);
    }
    printf("  Loaded %u bytes of program into %u cells\n", len, len < N_CELLS ? len : N_CELLS);
}

int main() {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║   POST-MOSFET — Storage and Logic in One Cell      ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");
    
    // ─── Create the fabric ───
    printf("═══ FABRIC CREATION ═══\n\n");
    fabric_t *f = create_fabric(64, 64);
    printf("  Cells: %d\n", N_CELLS);
    printf("  States per cell: %d\n", N_STATES);
    printf("  Total state bits: %d\n", N_CELLS * 8);
    printf("  Total transition entries: %d\n", N_CELLS * N_STATES);
    printf("  Input size: %u bytes\n", f->input_len);
    printf("  Output size: %u bytes\n\n", f->output_len);
    
    // ─── Load a program ───
    printf("═══ LOADING PROGRAM ═══\n\n");
    
    // The "program" is just data. It defines how cells transition.
    // This program: fibonacci sequence as cell transition patterns.
    uint8_t program[] = {
        1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 121, 98, 219,
        61, 24, 85, 109, 194, 47, 241, 32, 17, 49, 66, 115, 181, 40, 221, 5
    };
    load_program(f, program, sizeof(program));
    
    // ─── Load input ───
    printf("\n═══ LOADING INPUT ═══\n\n");
    uint8_t input_data[] = {42, 7, 3, 128, 255, 0, 196, 88, 12, 99};
    load_input(f, input_data, sizeof(input_data));
    printf("  Input: ");
    for (int i = 0; i < sizeof(input_data); i++) printf("%d ", input_data[i]);
    printf("\n  (%u bytes loaded into fabric)\n\n", (uint32_t)sizeof(input_data));
    
    // ─── Run ───
    printf("═══ COMPUTATION ═══\n\n");
    run(f, 10000);
    
    // ─── Read output ───
    printf("\n═══ READING OUTPUT ═══\n\n");
    uint8_t output[128] = {0};
    read_output(f, output, sizeof(output));
    printf("  Output: ");
    for (int i = 0; i < 16; i++) printf("%d ", output[i]);
    printf("...\n");
    
    // ─── Show cell state after computation ───
    printf("\n═══ FABRIC STATE ═══\n\n");
    printf("  Clock ticks: %u\n", f->clock);
    printf("  Total transitions: %lu\n", f->total_transitions);
    printf("  Total energy: %.3f nJ (%.3f uJ)\n",
           f->total_energy_pj / 1000.0,
           f->total_energy_pj / 1000000.0);
    printf("  Energy per transition: %.3f fJ\n",
           f->total_transitions > 0 ? (double)f->total_energy_pj / f->total_transitions : 0);
    
    printf("\n  First 16 cell states after computation:\n  ");
    for (int i = 0; i < 16; i++) {
        printf("[%d]=%d ", i, f->cells[i].state);
    }
    printf("\n\n  Last 16 cell states (output region):\n  ");
    for (int i = N_CELLS - 16; i < N_CELLS; i++) {
        printf("[%d]=%d ", i, f->cells[i].state);
    }
    
    printf("\n\n╔══════════════════════════════════════════════════════╗\n");
    printf("║   The program IS the data. The data IS the result.  ║\n");
    printf("║   No fetch. No decode. No execute.                 ║\n");
    printf("║   Just cells, transitioning, together.             ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    
    free(f->input);
    free(f->output);
    free(f);
    return 0;
}
