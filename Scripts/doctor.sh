#!/usr/bin/env bash
# Mocara preflight. Emits KEY=STATUS|detail lines for the editor to parse.
# Deliberately does NOT use `set -e`: every check must run even if an earlier one fails.

VENV="${VENV:-$HOME/.venvs/mocara}"
PY="$VENV/bin/python"

# Non-login shell: the profile is not sourced, so tools installed to ~/.local/bin
# (uv, huggingface-cli) are invisible unless we add them ourselves. Mirrors run_sidecar.sh.
export PATH="$HOME/.local/bin:$VENV/bin:$PATH"

emit() { printf 'MOCARA_CHECK %s=%s|%s\n' "$1" "$2" "$3"; }

# --- toolchain -------------------------------------------------------------
if command -v uv >/dev/null 2>&1; then
  emit uv OK "$(uv --version 2>/dev/null | head -1)"
else
  emit uv MISSING "install with: curl -LsSf https://astral.sh/uv/install.sh | sh"
fi

if command -v git >/dev/null 2>&1; then
  emit git OK "$(git --version 2>/dev/null)"
else
  emit git MISSING "sudo apt install git"
fi

# --- python env ------------------------------------------------------------
if [[ -x "$PY" ]]; then
  emit venv OK "$("$PY" -c 'import sys;print(sys.version.split()[0])' 2>/dev/null)"
else
  emit venv MISSING "run setup_kimodo.sh"
fi

# --- gpu -------------------------------------------------------------------
if [[ -x "$PY" ]]; then
  GPU="$("$PY" - <<'PY' 2>/dev/null
try:
    import torch
    if not torch.cuda.is_available():
        print("FAIL|torch present but CUDA unavailable (generation will be very slow)")
    else:
        major, minor = torch.cuda.get_device_capability(0)
        sm = f"sm_{major}{minor}"
        archs = torch.cuda.get_arch_list()
        name = torch.cuda.get_device_name(0)
        if sm in archs:
            print(f"OK|{name} {sm}, torch {torch.__version__}")
        else:
            # torch.cuda.is_available() is True here but there are no kernels for this
            # card, so every op fails or silently falls back. This is the failure mode
            # that looks like "it works but is mysteriously broken".
            print(f"FAIL|{name} is {sm} but this torch ({torch.__version__}) has no "
                  f"kernels for it (built for {' '.join(archs)}). Re-run Setup to install "
                  f"a matching CUDA build.")
except ImportError:
    print("MISSING|torch not installed")
except Exception as exc:
    print(f"FAIL|{exc}")
PY
)"
  emit gpu "${GPU%%|*}" "${GPU#*|}"
else
  emit gpu SKIP "no venv"
fi

# --- kimodo ----------------------------------------------------------------
if [[ -x "$PY" ]]; then
  K="$("$PY" - <<'PY' 2>/dev/null
try:
    from kimodo import DEFAULT_MODEL
    print(f"OK|default model {DEFAULT_MODEL}")
except Exception as exc:
    print(f"MISSING|{type(exc).__name__}: {exc}")
PY
)"
  emit kimodo "${K%%|*}" "${K#*|}"
else
  emit kimodo SKIP "no venv"
fi

# --- hugging face auth -----------------------------------------------------
TOKEN="$HOME/.cache/huggingface/token"
if [[ -s "$TOKEN" ]]; then
  emit hf_token OK "$(wc -c < "$TOKEN") bytes"
else
  emit hf_token MISSING "set HF_TOKEN on Windows, or log in with: huggingface-cli login"
fi

# --- gated Llama 3 access (the one step that genuinely needs a human) -------
if [[ -x "$PY" && -s "$TOKEN" ]]; then
  L="$("$PY" - <<'PY' 2>/dev/null
try:
    from huggingface_hub import hf_hub_download
    hf_hub_download("meta-llama/Meta-Llama-3-8B-Instruct", "config.json")
    print("OK|gated repo readable")
except Exception as exc:
    print(f"FAIL|{type(exc).__name__} - accept the licence at "
          f"huggingface.co/meta-llama/Meta-Llama-3-8B-Instruct while signed in as this token's account")
PY
)"
  emit llama3 "${L%%|*}" "${L#*|}"
else
  emit llama3 SKIP "needs venv + token"
fi

emit done OK "preflight complete"
