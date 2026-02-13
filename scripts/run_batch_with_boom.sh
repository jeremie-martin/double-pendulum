#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BATCH_CONFIG="$ROOT_DIR/config/batch.toml"
BOOM_REPO="$ROOT_DIR/../boom-detection"
BOOM_MODEL="models/best_10pct"
PENDULUM_BIN="$ROOT_DIR/build/pendulum"
PYTHON_BIN="python"
PYTHON_BIN_EXPLICIT=0
WAIT_SECONDS=20
KEEP_SERVER=0
NO_BUILD=0
PENDULUM_ARGS=()
SERVER_PID=""
PYTHONPATH_PREFIX=""

die() {
    echo "Error: $*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Run batch generation with boom_server.py automatically.

Usage:
  scripts/run_batch_with_boom.sh [launcher options] [-- pendulum args]

Launcher options:
  --config <path>       Batch TOML (default: config/batch.toml)
  --boom-repo <path>    boom-detection repo path (default: ../boom-detection)
  --model <path>        Model path passed to boom_server.py (default: models/best_10pct)
  --pendulum <path>     pendulum binary (default: build/pendulum)
  --python <bin>        Python interpreter (default: python, falls back to python3)
  --wait-seconds <n>    Wait timeout for boom socket (default: 20)
  --no-build            Do not auto-build if pendulum binary is missing
  --keep-server         Leave boom server running after pendulum exits
  -h, --help            Show this help

Examples:
  scripts/run_batch_with_boom.sh
  scripts/run_batch_with_boom.sh -- --resume
  scripts/run_batch_with_boom.sh --config config/batch.toml -- --set output.directory=batch_output/test
EOF
}

while (($#)); do
    case "$1" in
        --config)
            [[ $# -ge 2 ]] || die "--config requires a value"
            BATCH_CONFIG="$2"
            shift 2
            ;;
        --boom-repo)
            [[ $# -ge 2 ]] || die "--boom-repo requires a value"
            BOOM_REPO="$2"
            shift 2
            ;;
        --model)
            [[ $# -ge 2 ]] || die "--model requires a value"
            BOOM_MODEL="$2"
            shift 2
            ;;
        --pendulum)
            [[ $# -ge 2 ]] || die "--pendulum requires a value"
            PENDULUM_BIN="$2"
            shift 2
            ;;
        --python)
            [[ $# -ge 2 ]] || die "--python requires a value"
            PYTHON_BIN="$2"
            PYTHON_BIN_EXPLICIT=1
            shift 2
            ;;
        --wait-seconds)
            [[ $# -ge 2 ]] || die "--wait-seconds requires a value"
            WAIT_SECONDS="$2"
            shift 2
            ;;
        --no-build)
            NO_BUILD=1
            shift
            ;;
        --keep-server)
            KEEP_SERVER=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            PENDULUM_ARGS+=("$@")
            break
            ;;
        *)
            # Unknown args are forwarded to pendulum.
            PENDULUM_ARGS+=("$1")
            shift
            ;;
    esac
done

if [[ "$BATCH_CONFIG" != /* ]]; then
    BATCH_CONFIG="$ROOT_DIR/$BATCH_CONFIG"
fi
if [[ "$BOOM_REPO" != /* ]]; then
    BOOM_REPO="$ROOT_DIR/$BOOM_REPO"
fi
if [[ "$PENDULUM_BIN" != /* ]]; then
    PENDULUM_BIN="$ROOT_DIR/$PENDULUM_BIN"
fi

[[ -f "$BATCH_CONFIG" ]] || die "Batch config not found: $BATCH_CONFIG"
[[ -d "$BOOM_REPO" ]] || die "Boom repo not found: $BOOM_REPO"
[[ -f "$BOOM_REPO/scripts/boom_server.py" ]] || die "Missing $BOOM_REPO/scripts/boom_server.py"

if [[ "$PYTHON_BIN_EXPLICIT" -eq 0 ]]; then
    if [[ -x "$BOOM_REPO/.venv/bin/python" ]]; then
        PYTHON_BIN="$BOOM_REPO/.venv/bin/python"
    fi
fi

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
    if command -v python3 >/dev/null 2>&1; then
        PYTHON_BIN="python3"
    else
        die "Python interpreter not found"
    fi
fi

PYTHONPATH_PREFIX="$BOOM_REPO/src"
if [[ -n "${PYTHONPATH:-}" ]]; then
    PYTHONPATH_PREFIX="$PYTHONPATH_PREFIX:$PYTHONPATH"
fi

if ! PYTHONPATH="$PYTHONPATH_PREFIX" "$PYTHON_BIN" - <<'PY' >/dev/null 2>&1
import boom_detection  # noqa: F401
PY
then
    die "Python at '$PYTHON_BIN' cannot import boom_detection. Use --python or set up $BOOM_REPO/.venv"
fi

if [[ ! -x "$PENDULUM_BIN" ]]; then
    if [[ "$NO_BUILD" -eq 1 ]]; then
        die "Pendulum binary missing and --no-build was set: $PENDULUM_BIN"
    fi
    echo "[launcher] Building pendulum binary..."
    cmake -B "$ROOT_DIR/build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$ROOT_DIR/build" -j
fi

PHASE2_ENABLED="false"
SOCKET_VALUE=""
IN_PHASE2=0
while IFS= read -r raw_line || [[ -n "$raw_line" ]]; do
    line="${raw_line%%#*}"  # strip comments
    if [[ "$line" =~ ^[[:space:]]*\[probe\.phase2\][[:space:]]*$ ]]; then
        IN_PHASE2=1
        continue
    fi
    if [[ "$line" =~ ^[[:space:]]*\[[^]]+\][[:space:]]*$ ]]; then
        IN_PHASE2=0
        continue
    fi
    if [[ "$IN_PHASE2" -eq 1 ]]; then
        if [[ "$line" =~ ^[[:space:]]*enabled[[:space:]]*=[[:space:]]*(true|false)[[:space:]]*$ ]]; then
            PHASE2_ENABLED="${BASH_REMATCH[1]}"
        elif [[ "$line" =~ ^[[:space:]]*socket[[:space:]]*=[[:space:]]*\"([^\"]+)\"[[:space:]]*$ ]]; then
            SOCKET_VALUE="${BASH_REMATCH[1]}"
        fi
    fi
done < "$BATCH_CONFIG"

cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        if [[ "$KEEP_SERVER" -eq 1 ]]; then
            echo "[launcher] Leaving boom server running (pid=$SERVER_PID)"
            return
        fi
        echo "[launcher] Stopping boom server (pid=$SERVER_PID)"
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

cd "$ROOT_DIR"

if [[ "$PHASE2_ENABLED" != "true" ]]; then
    echo "[launcher] probe.phase2.enabled is not true; running batch without boom server."
    exec "$PENDULUM_BIN" "$BATCH_CONFIG" "${PENDULUM_ARGS[@]}"
fi

[[ -n "$SOCKET_VALUE" ]] || die "probe.phase2.enabled=true but no [probe.phase2].socket in $BATCH_CONFIG"
if [[ "$SOCKET_VALUE" = /* ]]; then
    SOCKET_PATH="$SOCKET_VALUE"
else
    SOCKET_PATH="$ROOT_DIR/$SOCKET_VALUE"
fi

mkdir -p "$(dirname "$SOCKET_PATH")"

SOCKET_REUSE=0
if [[ -S "$SOCKET_PATH" ]]; then
    if PYTHONPATH="$PYTHONPATH_PREFIX" "$PYTHON_BIN" - "$SOCKET_PATH" <<'PY'
import socket
import sys

path = sys.argv[1]
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(0.2)
try:
    sock.connect(path)
except OSError:
    sys.exit(1)
finally:
    sock.close()
sys.exit(0)
PY
    then
        SOCKET_REUSE=1
        echo "[launcher] Reusing existing boom server on $SOCKET_PATH"
    fi
fi

if [[ "$SOCKET_REUSE" -eq 0 ]]; then
    if [[ -S "$SOCKET_PATH" || -f "$SOCKET_PATH" ]]; then
        rm -f "$SOCKET_PATH"
    fi

    echo "[launcher] Starting boom server..."
    (
        cd "$BOOM_REPO"
        PYTHONPATH="$PYTHONPATH_PREFIX" "$PYTHON_BIN" scripts/boom_server.py --socket "$SOCKET_PATH" "$BOOM_MODEL"
    ) &
    SERVER_PID=$!

    deadline=$((SECONDS + WAIT_SECONDS))
    while ((SECONDS < deadline)); do
        if [[ -S "$SOCKET_PATH" ]]; then
            break
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            wait "$SERVER_PID" || true
            die "boom_server.py exited before socket became ready"
        fi
        sleep 0.1
    done

    [[ -S "$SOCKET_PATH" ]] || die "Timed out waiting for boom socket: $SOCKET_PATH"
fi

echo "[launcher] Boom server ready on $SOCKET_PATH"

echo "[launcher] Running batch..."
"$PENDULUM_BIN" "$BATCH_CONFIG" "${PENDULUM_ARGS[@]}"
