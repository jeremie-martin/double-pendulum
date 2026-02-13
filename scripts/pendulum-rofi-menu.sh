#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCHEDULE="$ROOT_DIR/scripts/pendulum-schedule.sh"

if ! command -v rofi >/dev/null 2>&1; then
    echo "Error: rofi is not installed." >&2
    exit 1
fi

if [[ ! -x "$SCHEDULE" ]]; then
    echo "Error: missing helper script: $SCHEDULE" >&2
    exit 1
fi

notify() {
    local msg="$1"
    if command -v notify-send >/dev/null 2>&1; then
        notify-send "Double Pendulum" "$msg"
    fi
}

choice="$(
    printf '%s\n' \
        'Status' \
        'Start now (manual)' \
        'Pause current run' \
        'Enable schedule timers' \
        'Disable all timers' \
        'Show recent logs' \
    | rofi -dmenu -i -p 'Pendulum'
)"

[[ -n "${choice:-}" ]] || exit 0

case "$choice" in
    'Status')
        status="$("$SCHEDULE" brief)"
        rofi -e "$status"
        ;;
    'Start now (manual)')
        "$SCHEDULE" start
        notify "Started manual run"
        ;;
    'Pause current run')
        "$SCHEDULE" pause
        notify "Paused current run"
        ;;
    'Enable schedule timers')
        "$SCHEDULE" enable
        notify "Enabled schedule timers"
        ;;
    'Disable all timers')
        "$SCHEDULE" disable
        notify "Disabled all timers"
        ;;
    'Show recent logs')
        logs="$(journalctl --user -u double-pendulum-manual.service -u double-pendulum-nightly.service -u double-pendulum-workday.service -n 25 --no-pager 2>&1 || true)"
        rofi -e "$logs"
        ;;
esac
