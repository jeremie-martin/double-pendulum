#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LAUNCHER="$ROOT_DIR/scripts/run_batch_with_boom_locked.sh"
BATCH_CONFIG="${ROOT_DIR}/config/batch.toml"

if [[ $# -gt 0 ]]; then
    BATCH_CONFIG="$1"
fi

if [[ "$BATCH_CONFIG" != /* ]]; then
    BATCH_CONFIG="$ROOT_DIR/$BATCH_CONFIG"
fi

if [[ ! -f "$BATCH_CONFIG" ]]; then
    echo "Error: batch config not found: $BATCH_CONFIG" >&2
    exit 1
fi

OUTPUT_DIR="$(
    python - "$BATCH_CONFIG" <<'PY'
import os
import pathlib
import sys

cfg_path = pathlib.Path(sys.argv[1])
with cfg_path.open('rb') as f:
    import tomllib
    data = tomllib.load(f)

output = data.get('batch', {}).get('output_directory', 'batch_output')
if not isinstance(output, str) or not output:
    output = 'batch_output'

if not os.path.isabs(output):
    output = str(pathlib.Path.cwd() / output)
print(output)
PY
)"

if find "$OUTPUT_DIR" -maxdepth 2 -type f -path '*/batch_*/progress.json' -print -quit 2>/dev/null | grep -q .; then
    exec "$LAUNCHER" --config "$BATCH_CONFIG" -- --resume
fi

exec "$LAUNCHER" --config "$BATCH_CONFIG"
