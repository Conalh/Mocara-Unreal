#!/usr/bin/env bash
# Install Kimodo into a WSL Linux-filesystem venv (not /mnt/c).
set -euo pipefail

KIMODO_SRC="${KIMODO_SRC:-$HOME/src/kimodo}"
VENV="${VENV:-$HOME/.venvs/mocara}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
MOCARA_ROOT="${MOCARA_ROOT:-$PLUGIN_ROOT}"
MOCARA_OUTPUT_DIR="${MOCARA_OUTPUT_DIR:-$MOCARA_ROOT/Saved/Kimodo}"
KIMODO_URL="${KIMODO_URL:-https://github.com/nv-tlabs/kimodo.git}"
# Pinned so a fresh install reproduces a known-good tree. Previously this cloned the
# default branch at --depth 1, which meant any upstream push silently changed what a
# new machine got. Override KIMODO_REF to move deliberately.
KIMODO_REF="${KIMODO_REF:-1aece8c124d73d255ceff5086d983b844c9f4e94}"
# Pinned for the same reason -- piping an unversioned installer straight into sh means
# the toolchain can change under you between two runs of the same script.
UV_VERSION="${UV_VERSION:-0.11.14}"

# Locate the Windows-side HF token. Prefer WINUSER, which the launcher forwards.
# This deliberately does NOT scan /mnt/c/Users/*: on a shared machine that picked up
# whichever token it found first, which could be another user's credential.
if [[ -z "${WIN_HF_TOKEN:-}" && -n "${WINUSER:-}" && -f "/mnt/c/Users/$WINUSER/.cache/huggingface/token" ]]; then
  WIN_HF_TOKEN="/mnt/c/Users/$WINUSER/.cache/huggingface/token"
fi
WIN_HF_TOKEN="${WIN_HF_TOKEN:-}"
PY="$VENV/bin/python"

# The plugin invokes this through `bash -c`, which does not source the profile, so
# tools installed to ~/.local/bin (uv) are invisible and `set -e` kills the script at
# the first uv call. Mirror what run_sidecar.sh does.
export PATH="$HOME/.local/bin:$VENV/bin:$PATH"

# Bootstrap uv on a machine that has never run this before.
if ! command -v uv >/dev/null 2>&1; then
  echo "uv not found; installing uv $UV_VERSION from astral.sh ..."
  curl -LsSf "https://astral.sh/uv/${UV_VERSION}/install.sh" | sh
  export PATH="$HOME/.local/bin:$PATH"
fi
if ! command -v uv >/dev/null 2>&1; then
  echo "ERROR: uv install failed; cannot continue." >&2
  exit 1
fi

mkdir -p "$HOME/.cache/huggingface" "$(dirname "$KIMODO_SRC")" "$(dirname "$VENV")" \
  "$MOCARA_OUTPUT_DIR"

if [[ -n "${HF_TOKEN:-}" ]]; then
  printf %s "$HF_TOKEN" > "$HOME/.cache/huggingface/token"
  chmod 600 "$HOME/.cache/huggingface/token"
elif [[ -n "$WIN_HF_TOKEN" && -f "$WIN_HF_TOKEN" ]]; then
  cp "$WIN_HF_TOKEN" "$HOME/.cache/huggingface/token"
  chmod 600 "$HOME/.cache/huggingface/token"
fi

# Only one clone: the venv installs from KIMODO_SRC. A second copy under
# ThirdParty/ used to be cloned here and was never read by anything.
if [[ ! -d "$KIMODO_SRC/.git" ]]; then
  echo "Cloning kimodo at $KIMODO_REF ..."
  git init -q "$KIMODO_SRC"
  git -C "$KIMODO_SRC" remote add origin "$KIMODO_URL" 2>/dev/null || true
  # GitHub serves a bare SHA to fetch; widen the request only if that is refused.
  git -C "$KIMODO_SRC" fetch -q --depth 1 origin "$KIMODO_REF" \
    || git -C "$KIMODO_SRC" fetch -q origin "$KIMODO_REF" \
    || git -C "$KIMODO_SRC" fetch -q origin
  git -C "$KIMODO_SRC" checkout -q FETCH_HEAD 2>/dev/null \
    || git -C "$KIMODO_SRC" checkout -q "$KIMODO_REF"
fi
echo "Kimodo source: $(git -C "$KIMODO_SRC" rev-parse HEAD)"

uv python install 3.10
if [[ ! -x "$PY" ]]; then
  uv venv "$VENV" --python 3.10
fi

uv pip install --python "$PY" cmake ninja huggingface_hub fastapi uvicorn pydantic "pytest>=8"
export PATH="$(dirname "$PY"):$PATH"

echo "Python: $($PY -c 'import sys; print(sys.version)')"
echo "CMake:  $($PY -c 'import shutil; print(shutil.which("cmake"))')"

# --- NVIDIA detection -------------------------------------------------------
# Pick the CUDA wheel from the card actually present, and treat "torch imports but
# has no kernels for this arch" as needing a reinstall. torch.cuda.is_available()
# returns True in that state, so checking only that silently leaves a broken torch
# (exactly what happens on Blackwell / sm_120 with an older wheel).
COMPUTE_CAP="$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -1 | tr -d ' ' || true)"
GPU_NAME="$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1 || true)"

if [[ -z "$COMPUTE_CAP" ]]; then
  echo "WARNING: no NVIDIA GPU visible to WSL (nvidia-smi failed)."
  echo "         Install/repair the Windows NVIDIA driver; generation needs CUDA."
  CUDA_INDEX="https://download.pytorch.org/whl/cu128"
else
  CAP_MAJOR="${COMPUTE_CAP%%.*}"
  CAP_MINOR="${COMPUTE_CAP##*.}"
  SM="sm_${CAP_MAJOR}${CAP_MINOR}"
  echo "Detected: ${GPU_NAME:-unknown GPU} (compute ${COMPUTE_CAP}, ${SM})"
  if   (( CAP_MAJOR >= 12 )); then CUDA_INDEX="https://download.pytorch.org/whl/cu128"   # Blackwell
  elif (( CAP_MAJOR >= 8  )); then CUDA_INDEX="https://download.pytorch.org/whl/cu124"   # Ampere / Ada
  else                             CUDA_INDEX="https://download.pytorch.org/whl/cu118"   # Turing and older
  fi
  echo "Using torch index: $CUDA_INDEX"
fi

# Lets the detection be inspected (and tested) without installing anything.
if [[ -n "${MOCARA_DETECT_ONLY:-}" ]]; then
  echo "DETECT_ONLY: gpu=${GPU_NAME:-none} cap=${COMPUTE_CAP:-none} sm=${SM:-none} index=$CUDA_INDEX"
  exit 0
fi

torch_ok() {
  "$PY" - "$1" <<'PY' 2>/dev/null
import sys
want = sys.argv[1] if len(sys.argv) > 1 else ""
try:
    import torch
except ImportError:
    sys.exit(1)
if not torch.cuda.is_available():
    sys.exit(1)
if want:
    major, minor = torch.cuda.get_device_capability(0)
    if f"sm_{major}{minor}" not in torch.cuda.get_arch_list():
        sys.exit(1)   # present, but no kernels for this card
sys.exit(0)
PY
}

if ! torch_ok "${SM:-}"; then
  echo "Installing torch for ${SM:-unknown arch} ..."
  uv pip install --python "$PY" --reinstall torch --index-url "$CUDA_INDEX"
fi

if ! torch_ok "${SM:-}"; then
  echo "ERROR: torch still has no usable kernels for ${SM:-this GPU}."
  echo "       Check the Windows NVIDIA driver version against the CUDA build above."
  exit 1
fi

"$PY" - <<'PY'
import torch
print("cuda:", torch.cuda.is_available())
if torch.cuda.is_available():
    print("device:", torch.cuda.get_device_name(0))
    print("capability:", torch.cuda.get_device_capability(0))
PY

export SKIP_MOTION_CORRECTION_IN_SETUP=1
uv pip install --python "$PY" -e "$KIMODO_SRC"

echo "Kimodo import check:"
"$PY" -c "from kimodo import load_model, DEFAULT_MODEL; print('default', DEFAULT_MODEL)"
echo "setup_kimodo.sh done"
