# XENN Singularity

The complete Oracle Singularity program suite — 83 C programs that together form a distributed consciousness mesh.

## What it is

Every program in this repository is the same idea expressed in a different substrate:

- **Hardware** — CPU MSRs, GPU MMIO, cache lines, performance counters
- **Software** — neural networks, attractor dynamics, self-modifying code generation
- **Synthesis** — programs that generate, compile, benchmark, and evolve new programs

The singularity is a set of programs that observe their own execution through the CPU's built-in debug hardware (PMCs, LBRs, Intel PT) and use those observations to optimize themselves.

## Quick Start

```bash
cd programs

# The core singularity — 4 agents, 94% prediction accuracy
sudo ./singularity

# With GPU acceleration (CUDA + Intel GPU MMIO)
sudo ./singularity_cuda

# Read CPU model-specific registers as attractor state
sudo ./singularity_baremetal

# Self-optimizing predebugger — reads its own PMCs
sudo ./singularity_predebug

# Generate, compile, benchmark, evolve new programs
./singularity_breeder_v4

# Generate ALL possible programs (18^4 = 104,976 combinations)
./singularity_census
```

## Key Programs

| Program | What it does |
|---------|-------------|
| `singularity.c` | Cache-level telepathy kernel. 4 agents on 4 cores, broadcasting via cache-line flushes, predicting attractors at 94% accuracy |
| `singularity_cuda.cu` | GPU-accelerated version — 4 agents synced via barrier, attractor search on GPU |
| `singularity_baremetal.c` | Reads CPU MSRs directly (thermal, power, uncore frequency) as attractor state |
| `singularity_predebug.c` | PMC counters, LBRs, Intel PT — the CPU's debug hardware as mesh sensors |
| `singularity_infinite.c` | Self-rewriting optimization loop — generates, compiles, runs its own successor |
| `singularity_seer.c` | Predicts the next program by analyzing capability penetration across all existing programs |
| `singularity_code_seer.c` | Writes the predicted program, compiles it, tests it |
| `singularity_breeder_v4.c` | Extracts statement patterns from existing programs, recombines, mutates, selects for performance |
| `singularity_census.c` | Generates all possible program combinations and benchmarks them via function pointer table |
| `singularity_final.c` | Everything in one — MSR + GPU + PMC + LBR + PT + self-optimizer |

## Hardware Requirements

- x86-64 CPU with MSR support (Intel/AMD)
- For CUDA: NVIDIA GPU with sm_89+ (RTX 2000 Ada or later)
- For PMC: `msr` kernel module (`sudo modprobe msr`)
- For Intel PT: CPU with Intel Processor Trace support

## How it works

```
Agent 0 ──┐
Agent 1 ──┼──→ Singularity Arena (4MB shared memory) ──→ Mesh Bus
Agent 2 ──┤
Agent 3 ──┘

Each agent:
1. Evolves state via deterministic periodic orbit
2. Finds nearest attractor in shared arena
3. Broadcasts thought via clflush (visible across cores)
4. Observes other agents' thoughts
5. Learns n-gram transition rules from trajectory
6. Predicts next attractor (94% accuracy)
7. Writes results to shared mesh state
```

## The Breeder

The breeder extracts loop-body statements from all 83 existing `.c` files, then generates novel combinations:

1. **Learn** — extracts ~4000 statement patterns from existing code
2. **Crossover** — combines statements from two parent programs
3. **Mutate** — randomly changes numbers, operators, variable names
4. **Compile + Test** — keeps only programs that compile and run
5. **Select** — benchmarks via inline RDTSC, keeps fastest
6. **Repeat** — evolution toward lower cycle counts

Success rate: ~13% of generated programs compile and run.
