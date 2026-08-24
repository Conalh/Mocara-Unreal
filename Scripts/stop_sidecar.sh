#!/usr/bin/env bash
set -euo pipefail

PIDFILE="${MOCARA_PIDFILE:-$HOME/.cache/mocara/sidecar.pid}"
if [[ ! -s "$PIDFILE" ]]; then
  rm -f "$PIDFILE"
  exit 0
fi

pid=""
owner_token=""
extra=""
read -r pid owner_token extra < "$PIDFILE" || true
rm -f "$PIDFILE"

case "$pid" in
  ''|*[!0-9]*) exit 0 ;;
esac
[[ "$owner_token" == mocara-* && -z "$extra" ]] || exit 0
[[ -r "/proc/$pid/environ" && -r "/proc/$pid/cmdline" ]] || exit 0

if ! tr '\0' '\n' < "/proc/$pid/environ" | grep -Fxq -- "MOCARA_SIDECAR_TOKEN=$owner_token"; then
  exit 0
fi
cmdline="$(tr '\0' ' ' < "/proc/$pid/cmdline")"
[[ "$cmdline" == *"python -m uvicorn mocara_sidecar.server:app"* ]] || exit 0

kill -TERM "$pid" 2>/dev/null || true
