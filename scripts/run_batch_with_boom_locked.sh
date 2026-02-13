#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK_ROOT="${XDG_RUNTIME_DIR:-/tmp}"
LOCK_FILE="$LOCK_ROOT/double-pendulum-batch.lock"

if ! command -v flock >/dev/null 2>&1; then
    exec "$ROOT_DIR/scripts/run_batch_with_boom.sh" "$@"
fi

mkdir -p "$LOCK_ROOT"
exec 9>"$LOCK_FILE"

if ! flock -n 9; then
    echo "[launcher] Another double-pendulum batch run is already active. Skipping duplicate start."
    exit 0
fi

exec "$ROOT_DIR/scripts/run_batch_with_boom.sh" "$@"
