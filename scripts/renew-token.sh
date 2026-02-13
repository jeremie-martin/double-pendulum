#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
  cat <<'EOF'
Usage: scripts/renew-token.sh [credentials_dir]

Renews YouTube OAuth credentials by forcing a fresh auth flow.
If credentials_dir is omitted, pendulum-tools config/defaults are used.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ $# -gt 1 ]]; then
  usage
  exit 1
fi

cd "${REPO_ROOT}"

if [[ $# -eq 1 ]]; then
  uv run pendulum-tools auth --force --credentials "$1"
else
  uv run pendulum-tools auth --force
fi
