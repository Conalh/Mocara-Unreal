#!/usr/bin/env bash
set -euo pipefail
VENV="${VENV:-$HOME/.venvs/mocara}"
if [[ -n "${MOCARA_ROOT:-}" ]]; then
  ROOT="$MOCARA_ROOT"
elif [[ -n "${BASH_SOURCE[0]:-}" ]]; then
  SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
  PLUGIN_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
  ROOT="$PLUGIN_ROOT"
else
  echo "MOCARA_ROOT is required when run_sidecar.sh is streamed over stdin" >&2
  exit 2
fi
export TEXT_ENCODER_DEVICE="${TEXT_ENCODER_DEVICE:-cpu}"
export TEXT_ENCODER_MODE="${TEXT_ENCODER_MODE:-local}"
TEXT_ENCODER_FP32="${TEXT_ENCODER_FP32:-1}"
export TEXT_ENCODER_FP32
export MOCARA_OUTPUT_DIR="${MOCARA_OUTPUT_DIR:-$ROOT/Saved/Kimodo}"
export PYTHONPATH="$ROOT/Sidecar:${PYTHONPATH:-}"
export PATH="$HOME/.local/bin:$VENV/bin:$PATH"
if [[ -n "${HF_TOKEN:-}" ]]; then
  mkdir -p "$HOME/.cache/huggingface"
  printf %s "$HF_TOKEN" > "$HOME/.cache/huggingface/token"
  chmod 600 "$HOME/.cache/huggingface/token"
fi
source "$VENV/bin/activate"
# The editor forwards the port from its SidecarUrl setting. Without this the script
# always bound 8765 while the client talked to whatever the setting said, so changing
# the setting pointed the plugin at a port nothing was listening on.
PORT="${MOCARA_PORT:-8765}"
case "$PORT" in
  ''|*[!0-9]*) PORT=8765 ;;
esac
PIDFILE="${MOCARA_PIDFILE:-$HOME/.cache/mocara/sidecar.pid}"
mkdir -p "$(dirname "$PIDFILE")"
SIDECAR_PID=""
OWNER_TOKEN=""
cleanup() {
  local status=$?
  local stored_pid=""
  local stored_token=""
  if [[ -n "$SIDECAR_PID" && -f "$PIDFILE" ]] && \
     read -r stored_pid stored_token < "$PIDFILE" && \
     [[ "$stored_pid" == "$SIDECAR_PID" && "$stored_token" == "$OWNER_TOKEN" ]]; then
    rm -f "$PIDFILE"
  fi
  return "$status"
}
forward_signal() {
  if [[ -n "$SIDECAR_PID" ]] && kill -0 "$SIDECAR_PID" 2>/dev/null; then
    kill -TERM "$SIDECAR_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT
trap forward_signal INT TERM
OWNER_TOKEN="mocara-$$-$(date +%s%N)-$RANDOM"
MOCARA_SIDECAR_TOKEN="$OWNER_TOKEN" \
  python -m uvicorn mocara_sidecar.server:app --host 127.0.0.1 --port "$PORT" &
SIDECAR_PID=$!
printf '%s %s\n' "$SIDECAR_PID" "$OWNER_TOKEN" > "$PIDFILE"
wait "$SIDECAR_PID"
