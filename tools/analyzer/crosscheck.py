#!/usr/bin/env python3
"""Cross-check every independent implementation against the C++ engine.

Given one authoritative (clean) event stream, this runs:

  * the C++ engine            (`marketstate replay`),
  * the Python reference      (reference/python/model.py),
  * the OCaml reference       (reference/ocaml/model.ml),

and asserts their canonical states are byte-identical. That all three agree is
what justifies calling the reference models *independent*: different languages,
different in-memory layouts, identical output.

Usage:
  tools/analyzer/crosscheck.py --events data/demo_clean.txt
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from reflib import REFERENCES, reference_command  # noqa: E402


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def digest(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()[:16]


def main() -> int:
    parser = argparse.ArgumentParser(description="MarketState cross-implementation check")
    parser.add_argument("--engine", default=os.path.join(ROOT, "build", "core", "marketstate"))
    parser.add_argument("--events", required=True, help="authoritative clean stream")
    parser.add_argument("--workdir", default=None)
    parser.add_argument("--report-out", default=None)
    args = parser.parse_args()

    workdir = args.workdir or tempfile.mkdtemp(prefix="marketstate-xcheck-")
    os.makedirs(workdir, exist_ok=True)

    hashes: dict[str, str] = {}

    engine_state = os.path.join(workdir, "engine.state")
    proc = run([args.engine, "replay", "--events", args.events,
                "--state-out", engine_state, "--quiet"])
    if proc.returncode != 0:
        raise RuntimeError(f"engine replay failed: {proc.stderr}")
    hashes["engine"] = digest(engine_state)

    for name in REFERENCES:
        state = os.path.join(workdir, f"{name}.state")
        proc = run(reference_command(name, args.events, state))
        if proc.returncode != 0:
            raise RuntimeError(f"{name} reference failed: {proc.stderr}")
        hashes[name] = digest(state)

    distinct = set(hashes.values())
    report = {
        "events": os.path.abspath(args.events),
        "hashes": hashes,
        "all_equal": len(distinct) == 1,
    }
    if args.report_out:
        with open(args.report_out, "w") as handle:
            json.dump(report, handle, indent=2)
    print(json.dumps(report, indent=2))
    return 0 if report["all_equal"] else 1


if __name__ == "__main__":
    sys.exit(main())
