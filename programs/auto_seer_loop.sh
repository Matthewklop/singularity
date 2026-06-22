#!/bin/bash
# AUTO-SEER LOOP — generates, compiles, tests, and feeds back
# Keeps going until the predictions stop changing

echo "═══ AUTO-SEER LOOP ═══"
echo "Generating, compiling, running, feeding back..."
echo ""

for gen in $(seq 0 20); do
    echo "── Generation $gen ──"
    
    # Run the code seer to predict and generate
    ./singularity_code_seer 2>&1 | grep -E "→|✅|❌|fuse"
    
    # Find the newest predicted file
    LATEST=$(ls -t predicted_*.c 2>/dev/null | head -1)
    if [ -z "$LATEST" ]; then
        echo "No prediction generated, stopping"
        break
    fi
    
    # Get the binary name (strip .c)
    BIN="${LATEST%.c}"
    
    # Test it under PMC
    if [ -f "$BIN" ]; then
        echo -n "  Benchmarking $BIN... "
        sudo modprobe msr 2>/dev/null
        # Run with perf stat if available
        PERF_OUT=$(sudo perf stat -e instructions,cycles,L1-dcache-load-misses -x ',' ./$BIN 2>&1 | tail -3)
        echo "$PERF_OUT" | head -1
    fi
    
    echo ""
    
    # Stop if we've generated a lot and the pattern is repeating
    if [ $gen -ge 15 ]; then
        # Check if the last 3 predictions are the same combo
        LAST3=$(ls -t predicted_*.c 2>/dev/null | head -3 | xargs -I{} basename {} .c)
        if [ "$(echo "$LAST3" | sort -u | wc -l)" -eq 1 ]; then
            echo "Converged at generation $gen"
            break
        fi
    fi
done

echo ""
echo "═══ FINAL PREDICTIONS ═══"
ls -la predicted_*.c 2>/dev/null | awk '{print $NF}'
echo ""
echo "Done"
