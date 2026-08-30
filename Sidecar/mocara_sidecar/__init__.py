"""Packaged Mocara sidecar runtime with a lazy application import."""

from __future__ import annotations

from typing import Any


__all__ = ["create_app"]


def __getattr__(name: str) -> Any:
    # Policy-only utilities such as the native benchmark use ``backends`` from
    # a plain Windows Python.  Do not require FastAPI merely to import that
    # standard-library-only module.
    if name == "create_app":
        from .server import create_app

        return create_app
    raise AttributeError(name)
