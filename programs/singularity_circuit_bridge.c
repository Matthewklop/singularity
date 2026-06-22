// singularity_circuit_bridge.c — Feed circuit truth tables into GPU singularity
// Compile: gcc -O3 -o singularity_circuit_bridge singularity_circuit_bridge.c -lm
// Run: ./singularity_circuit_bridge | ./singularity_cuda

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// Truth table -> attractor seed format
// Each row: [A, B, Carry, Sum] -> 4 attractor coordinates
typedef struct {
    const char *name;
    int n_inputs;
    int n_outputs;
    float table[8][8];  // max 8 rows x 8 cols
    int n_rows;
} circuit_truth_t;

static circuit_truth_t circuits[] = {
    {"half_adder", 2, 2, {
        {0,0,0,0},
        {0,1,0,1},
        {1,0,0,1},
        {1,1,1,0}
    }, 4},
    {"full_adder", 3, 2, {
        {0,0,0,0,0},
        {0,0,1,0,1},
        {0,1,0,0,1},
        {0,1,1,1,0},
        {1,0,0,0,1},
        {1,0,1,1,0},
        {1,1,0,1,0},
        {1,1,1,1,1}
    }, 8},
    {"nand", 2, 1, {
        {0,0,1},
        {0,1,1},
        {1,0,1},
        {1,1,0}
    }, 4},
    {"xor", 2, 1, {
        {0,0,0},
        {0,1,1},
        {1,0,1},
        {1,1,0}
    }, 4},
};

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "half_adder";
    circuit_truth_t *ct = NULL;
    
    for (int i = 0; i < sizeof(circuits)/sizeof(circuits[0]); i++) {
        if (strcmp(circuits[i].name, name) == 0) { ct = &circuits[i]; break; }
    }
    if (!ct) {
        fprintf(stderr, "Unknown circuit: %s\n", name);
        fprintf(stderr, "Known: half_adder, full_adder, nand, xor\n");
        return 1;
    }
    
    printf("╔══════════════════════════════════════╗\n");
    printf("║  CIRCUIT → SINGULARITY BRIDGE       ║\n");
    printf("╚══════════════════════════════════════╝\n\n");
    printf("Circuit: %s\n", ct->name);
    printf("Truth table (%d rows):\n", ct->n_rows);
    
    int total_dims = ct->n_inputs + ct->n_outputs;
    
    for (int r = 0; r < ct->n_rows; r++) {
        printf("  ");
        for (int i = 0; i < ct->n_inputs; i++)
            printf("%s%.0f", i?" ":"", ct->table[r][i]);
        printf(" → ");
        for (int i = 0; i < ct->n_outputs; i++)
            printf("%s%.0f", i?" ":"", ct->table[r][ct->n_inputs + i]);
        printf("\n");
    }
    
    printf("\nFeeding %d attractor seeds into singularity...\n", ct->n_rows);
    printf("Each row = 1 attractor with %d-D state vector\n", total_dims);
    printf("\nPrediction target: given inputs, predict outputs\n");
    printf("=== END BRIDGE ===\n");
    
    // Output format for parsing by simulation wrapper
    for (int r = 0; r < ct->n_rows; r++) {
        printf("[SEED]");
        for (int i = 0; i < total_dims; i++)
            printf(" %.3f", ct->table[r][i]);
        printf("\n");
    }
    
    // Test all input combinations and predict
    printf("\n[PREDICTIONS]\n");
    int test_cases = 1 << ct->n_inputs;
    for (int t = 0; t < test_cases; t++) {
        printf("[TEST]");
        for (int i = 0; i < ct->n_inputs; i++)
            printf(" %d", (t >> i) & 1);
        printf("\n");
    }
    
    return 0;
}
