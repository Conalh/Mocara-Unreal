from pathlib import Path
import sys

import pytest
from pydantic import ValidationError


sys.path.insert(
    0,
    str(Path(__file__).resolve().parents[1] / "Sidecar"),
)

from mocara_sidecar.server import GenerateRequest, _wsl_to_windows


def test_wsl_path_converts_to_windows_drive_on_any_host() -> None:
    converted = _wsl_to_windows(
        Path("/mnt/c/Users/example/MocaraProject/Saved/Kimodo/x.bvh")
    )

    assert converted == "C:/Users/example/MocaraProject/Saved/Kimodo/x.bvh"


@pytest.mark.parametrize("prompt", ["", "   ", "\t\n"])
def test_generate_request_rejects_blank_prompt(prompt: str) -> None:
    with pytest.raises(ValidationError):
        GenerateRequest(prompt=prompt)


@pytest.mark.parametrize("duration", [0, -1, 0.49, 30.1])
def test_generate_request_bounds_duration(duration: float) -> None:
    with pytest.raises(ValidationError):
        GenerateRequest(prompt="walk", duration=duration)


@pytest.mark.parametrize("steps", [0, -1, 501])
def test_generate_request_bounds_diffusion_steps(steps: int) -> None:
    with pytest.raises(ValidationError):
        GenerateRequest(prompt="walk", diffusion_steps=steps)


def test_generate_request_trims_prompt() -> None:
    request = GenerateRequest(prompt="  walk forward  ")

    assert request.prompt == "walk forward"
