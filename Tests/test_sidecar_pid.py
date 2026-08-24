import os
from pathlib import Path
import shutil
import subprocess

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]


def _bash_path(path: Path) -> str:
    if os.name != "nt":
        return path.as_posix()
    return f"/mnt/{path.drive[0].lower()}/{path.as_posix().split(':', maxsplit=1)[1].lstrip('/')}"


def _run_bash(script: str, *args: str) -> subprocess.CompletedProcess[bytes]:
    if os.name == "nt":
        if not shutil.which("wsl.exe"):
            pytest.skip("WSL is required for sidecar lifecycle tests on Windows")
        command = [
            "wsl.exe",
            "-d",
            os.environ.get("MOCARA_TEST_WSL_DISTRO", "Ubuntu"),
            "--",
            "bash",
            "-s",
            "--",
            *args,
        ]
    else:
        if not shutil.which("bash"):
            pytest.skip("bash is required for sidecar lifecycle tests")
        command = ["bash", "-s", "--", *args]

    # Bytes avoid Windows text-mode pipes rewriting LF to CRLF before WSL reads stdin.
    return subprocess.run(
        command,
        input=script.replace("\r\n", "\n").encode("utf-8"),
        capture_output=True,
        check=False,
    )


def _output(result: subprocess.CompletedProcess[bytes]) -> str:
    return (result.stdout + result.stderr).decode("utf-8", errors="replace")


@pytest.mark.parametrize(
    ("configured", "expected"),
    [("", "8765"), ("9123", "9123"), ("not-a-port", "8765")],
)
def test_run_script_binds_the_forwarded_port(configured: str, expected: str) -> None:
    """The editor forwards MOCARA_PORT from SidecarUrl; check uvicorn actually gets it.

    Asserts on the argv the script hands to python, not on the text of the script, so a
    refactor that keeps the behaviour keeps passing.
    """
    run_script = _bash_path(REPO_ROOT / "Scripts" / "run_sidecar.sh")
    result = _run_bash(
        r'''
set -euo pipefail
run_script="$1"
configured="$2"
test_root="$(mktemp -d)"
trap 'rm -rf "$test_root"' EXIT
mkdir -p "$test_root/venv/bin"
printf ':\n' > "$test_root/venv/bin/activate"
# Stub python records how it was invoked instead of starting a server.
printf '#!/usr/bin/env bash\necho "$@" > "%s/argv"\n' "$test_root" > "$test_root/venv/bin/python"
chmod +x "$test_root/venv/bin/python"
VENV="$test_root/venv" MOCARA_ROOT="$test_root/root" MOCARA_PIDFILE="$test_root/sidecar.pid" \
  MOCARA_PORT="$configured" bash "$run_script"
cat "$test_root/argv"
''',
        run_script,
        configured,
    )

    assert result.returncode == 0, _output(result)
    argv = _output(result).strip()
    assert f"--port {expected}" in argv, argv
    assert "--host 127.0.0.1" in argv, argv


def test_run_script_starts_when_streamed_over_stdin_by_the_editor() -> None:
    """The Unreal launcher CR-strips the packaged script and pipes it to Bash."""
    run_script = _bash_path(REPO_ROOT / "Scripts" / "run_sidecar.sh")
    result = _run_bash(
        r'''
set -euo pipefail
run_script="$1"
test_root="$(mktemp -d)"
trap 'rm -rf "$test_root"' EXIT
mkdir -p "$test_root/venv/bin"
printf ':\n' > "$test_root/venv/bin/activate"
printf '#!/usr/bin/env bash\necho "$@" > "%s/argv"\n' "$test_root" > "$test_root/venv/bin/python"
chmod +x "$test_root/venv/bin/python"
sed 's/\r$//' "$run_script" | \
  VENV="$test_root/venv" MOCARA_ROOT="$test_root/root" MOCARA_PIDFILE="$test_root/sidecar.pid" \
  bash
cat "$test_root/argv"
''',
        run_script,
    )

    assert result.returncode == 0, _output(result)
    assert "-m uvicorn mocara_sidecar.server:app" in _output(result)


def test_run_script_removes_pidfile_after_the_sidecar_exits() -> None:
    run_script = _bash_path(REPO_ROOT / "Scripts" / "run_sidecar.sh")
    result = _run_bash(
        r'''
set -euo pipefail
run_script="$1"
test_root="$(mktemp -d)"
trap 'rm -rf "$test_root"' EXIT
mkdir -p "$test_root/venv/bin"
printf ':\n' > "$test_root/venv/bin/activate"
printf '#!/usr/bin/env bash\nsleep 0.1\n' > "$test_root/venv/bin/python"
chmod +x "$test_root/venv/bin/python"
VENV="$test_root/venv" MOCARA_ROOT="$test_root/root" MOCARA_PIDFILE="$test_root/sidecar.pid" \
  bash "$run_script"
test ! -e "$test_root/sidecar.pid"
''',
        run_script,
    )

    assert result.returncode == 0, _output(result)


def test_stop_script_ignores_stale_owner_and_stops_matching_owner() -> None:
    stop_script = _bash_path(REPO_ROOT / "Scripts" / "stop_sidecar.sh")
    result = _run_bash(
        r'''
set -euo pipefail
stop_script="$1"
test_root="$(mktemp -d)"
unrelated_pid=""
owned_pid=""
cleanup() {
  [[ -z "$unrelated_pid" ]] || kill "$unrelated_pid" 2>/dev/null || true
  [[ -z "$owned_pid" ]] || kill "$owned_pid" 2>/dev/null || true
  rm -rf "$test_root"
}
trap cleanup EXIT

sleep 30 &
unrelated_pid=$!
printf '%s %s\n' "$unrelated_pid" stale-owner > "$test_root/sidecar.pid"
MOCARA_PIDFILE="$test_root/sidecar.pid" bash "$stop_script"
kill -0 "$unrelated_pid"
test ! -e "$test_root/sidecar.pid"

owner_token="mocara-test-$RANDOM-$RANDOM"
MOCARA_SIDECAR_TOKEN="$owner_token" \
  bash -c 'exec -a "python -m uvicorn mocara_sidecar.server:app" sleep 30' &
owned_pid=$!
printf '%s %s\n' "$owned_pid" "$owner_token" > "$test_root/sidecar.pid"
MOCARA_PIDFILE="$test_root/sidecar.pid" bash "$stop_script"

for _ in {1..50}; do
  if ! kill -0 "$owned_pid" 2>/dev/null; then
    owned_pid=""
    break
  fi
  sleep 0.02
done
test -z "$owned_pid"
test ! -e "$test_root/sidecar.pid"
''',
        stop_script,
    )

    assert result.returncode == 0, _output(result)
