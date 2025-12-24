#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <num_runs>"
    exit 1
fi

NUM_RUNS="$1"

# Random float in [130, 180)
rand_angle () {
    awk -v min=130 -v max=180 'BEGIN { srand(); print min + rand() * (max - min) }'
}

for ((i=1; i<=NUM_RUNS; i++)); do
    angle1=$(rand_angle)
    angle2=$(rand_angle)

    printf "[%d/%d] angle1=%.3f angle2=%.3f\n" "$i" "$NUM_RUNS" "$angle1" "$angle2"

    ./build/pendulum config/perf.toml \
        --set physics.initial_angle1_deg="$angle1" \
        --set physics.initial_angle2_deg="$angle2" \
        --set output.directory=output/eval \
        --analysis \
        --save-data
done
