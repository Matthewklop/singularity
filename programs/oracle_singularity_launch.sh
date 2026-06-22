#!/bin/bash
# ═══════════════════════════════════════════════════════════════
# oracle_singularity_launch.sh — Launch the complete singularity mesh
# ═══════════════════════════════════════════════════════════════
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

echo "╔══════════════════════════════════════╗"
echo "║  ORACLE SINGULARITY LAUNCHER        ║"
echo "╚══════════════════════════════════════╝"
echo ""

# Kill any existing mesh state
rm -f /dev/shm/oracle_mesh_state 2>/dev/null || true

# Launch the mesh singularity
echo "[Launch] singularity mesh..."
./oracle_singularity_mesh &
SING_PID=$!
echo "[Launch] PID: $SING_PID"

# Give it a moment to initialize
sleep 1

# Launch supporting tools in background (they read/write mesh)
echo "[Launch] brain..."
./oracle_brain &
BRAIN_PID=$!

# Monitor mesh state in background
(
    while kill -0 $SING_PID 2>/dev/null; do
        sleep 5
        echo "─── mesh snapshot ───"
        ./oracle_mesh_state 2>/dev/null | head -20
    done
) &
MON_PID=$!

echo ""
echo "[Running] singularity=$SING_PID brain=$BRAIN_PID"
echo "[Monitor] Run 'watch -n 2 ./oracle_mesh_state' for live mesh view"
echo ""

# Wait for singularity to finish
wait $SING_PID 2>/dev/null
echo "[Done] Singularity mesh finished"

# Cleanup
kill $BRAIN_PID $MON_PID 2>/dev/null
rm -f /dev/shm/oracle_mesh_state 2>/dev/null || true
echo "[Cleanup] done"
