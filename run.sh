#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <num_runs>"
    exit 1
fi

NUM_RUNS="$1"

for ((i=1; i<=NUM_RUNS; i++)); do
    # Seed randomness once per run
    seed=$((RANDOM + i))

    # angle1=$(awk -v s="$seed" 'BEGIN { srand(s); print 130 + rand() * 50 }')
    # angle2=$(awk -v s="$((seed+1))" 'BEGIN { srand(s); print 130 + rand() * 50 }')

    angle1=$(awk -v s="$seed" 'BEGIN { srand(s); print 189.5 + rand() }')
    angle2=$(awk -v s="$((seed+1))" 'BEGIN { srand(s); print 189.5 + rand() }')

    pendulum_count=$(awk -v s="$((seed+2))" 'BEGIN { srand(s); print int(2000 + rand() * (5000 - 2000 + 1)) }')
    duration_seconds=$(awk -v s="$((seed+3))" 'BEGIN { srand(s); print int(15 + rand() * (20 - 15 + 1)) }')
    total_frames=$(awk -v s="$((seed+4))" 'BEGIN { srand(s); print int(1000 + rand() * (1500 - 1000 + 1)) }')

    printf "[%d/%d] angle1=%.3f angle2=%.3f pendulums=%d duration=%ds frames=%d\n" \
        "$i" "$NUM_RUNS" "$angle1" "$angle2" \
        "$pendulum_count" "$duration_seconds" "$total_frames"

    ./build/pendulum config/perf.toml \
        --set physics.initial_angle1_deg="180.1" \
        --set physics.initial_angle2_deg="179.1" \
        --set simulation.pendulum_count="5000" \
        --set simulation.duration_seconds="20" \
        --set simulation.total_frames="1800" \
        --set output.directory=output/eval3 \
        --analysis \
        --save-data
done
