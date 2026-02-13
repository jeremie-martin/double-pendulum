#!/usr/bin/env bash
set -euo pipefail

SERVICES=(
  double-pendulum-nightly.service
  double-pendulum-workday.service
  double-pendulum-manual.service
)

TIMERS=(
  double-pendulum-nightly.timer
  double-pendulum-workday.timer
)

usage() {
  cat <<'EOF'
Usage: scripts/pendulum-schedule.sh <command>

Commands:
  start      Start a manual run now (continues until paused/stopped)
  pause      Stop any currently running jobs (next schedule still runs)
  stop       Alias for pause
  status     Show timer schedule + current unit status
  brief      Print a short status summary (for menus/scripts)
  enable     Enable and start both timers
  disable    Disable both timers and stop running jobs
EOF
}

cmd="${1:-status}"

case "$cmd" in
  start|start-now|run-now)
    systemctl --user daemon-reload
    systemctl --user start double-pendulum-manual.service
    echo "Started manual run. Use 'scripts/pendulum-schedule.sh pause' to stop."
    ;;
  pause)
    systemctl --user stop "${SERVICES[@]}"
    echo "Paused current run(s). Next scheduled runs remain enabled."
    ;;
  stop)
    systemctl --user stop "${SERVICES[@]}"
    echo "Stopped current run(s). Next scheduled runs remain enabled."
    ;;
  status)
    echo "Timers:"
    systemctl --user list-timers --all --no-pager | rg 'double-pendulum-(nightly|workday)|NEXT|LEFT|^$' || true
    echo
    echo "Units:"
    systemctl --user --no-pager --full status "${TIMERS[@]}" "${SERVICES[@]}" || true
    ;;
  brief)
    echo "Next timers:"
    systemctl --user list-timers --all --no-pager \
      | rg 'double-pendulum-(nightly|workday)|NEXT|LEFT' || true
    echo
    echo "Running services:"
    systemctl --user list-units --type=service --state=running --no-pager \
      | rg 'double-pendulum-(nightly|workday|manual)\\.service' || echo "(none)"
    ;;
  enable)
    systemctl --user daemon-reload
    systemctl --user enable --now "${TIMERS[@]}"
    echo "Enabled timers."
    ;;
  disable)
    systemctl --user disable --now "${TIMERS[@]}"
    systemctl --user stop "${SERVICES[@]}" || true
    echo "Disabled timers and stopped active runs."
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    usage
    exit 1
    ;;
esac
