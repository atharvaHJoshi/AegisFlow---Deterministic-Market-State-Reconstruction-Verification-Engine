#!/usr/bin/env python3
"""Locate an independent reference-model implementation for MarketState.

Two are available and must produce byte-identical canonical state:

  * python : reference/python/model.py
  * ocaml  : reference/ocaml/model.ml (built on demand via build.sh)

`reference_command` returns the argv to run a given implementation with the
same CLI used throughout the harness (`--events`, `--state-out`,
`--summary-out`), so callers do not need to care which is selected.
"""

from __future__ import annotations

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

PY_MODEL = os.path.join(ROOT, "reference", "python", "model.py")
OCAML_BUILD = os.path.join(ROOT, "reference", "ocaml", "build.sh")
OCAML_MODEL = os.path.join(ROOT, "reference", "ocaml", "_build", "model")

REFERENCES = ("python", "ocaml")


def ensure_ocaml_model() -> str:
    """Build the OCaml model if needed and return its path."""
    if not os.path.exists(OCAML_MODEL):
        proc = subprocess.run([OCAML_BUILD], capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(
                "failed to build OCaml reference model:\n" + proc.stderr
            )
    return OCAML_MODEL


def reference_command(
    name: str, events: str, state_out: str, summary_out: str | None = None
) -> list[str]:
    if name == "python":
        cmd = [sys.executable, PY_MODEL]
    elif name == "ocaml":
        cmd = [ensure_ocaml_model()]
    else:
        raise ValueError(f"unknown reference implementation: {name!r}")
    cmd += ["--events", events, "--state-out", state_out]
    if summary_out:
        cmd += ["--summary-out", summary_out]
    return cmd
