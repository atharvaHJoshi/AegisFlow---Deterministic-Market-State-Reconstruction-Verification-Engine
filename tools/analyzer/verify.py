#!/usr/bin/env python3
"""End-to-end verification harness for MarketState.

Runs the C++ engine and the independent Python reference model over the same
authoritative history and checks:

  1. Differential correctness  : engine(clean)        == reference(clean)
  2. Deterministic replay      : engine(clean) twice   == each other
  3. Recovery equivalence      : engine(chaos + retransmit) == reference(clean)
  4. Unreliable-without-source : engine(chaos, no source) reports unresolved gaps
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from reflib import REFERENCES, reference_command  # noqa: E402


def run(cmd):
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.returncode, proc.stdout, proc.stderr


def read(path):
    with open(path, "r") as handle:
        return handle.read()


def content_hash(text):
    return hashlib.sha256(text.encode()).hexdigest()[:16]


def reference_state(events_path, out_path, reference="python"):
    cmd = reference_command(reference, events_path, out_path)
    code, out, err = run(cmd)
    if code != 0:
        raise RuntimeError(f"reference model ({reference}) failed: {err}")
    return json.loads(out)


def engine_replay(engine, events_path, out_path):
    code, out, err = run([engine, "replay", "--events", events_path, "--state-out", out_path,
                          "--quiet"])
    if code != 0:
        raise RuntimeError(f"engine replay failed: {err}")
    return out.strip()


def engine_run(engine, events_path, out_path, recovery_source=None):
    cmd = [engine, "run", "--events", events_path, "--state-out", out_path]
    if recovery_source:
        cmd += ["--recovery-source", recovery_source]
    code, out, err = run(cmd + ["--quiet"])
    if code != 0:
        # exit code 2 means recovered state differed; still captured on files
        if code != 2:
            raise RuntimeError(f"engine run failed: {err}")
    return out.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description="MarketState verification")
    parser.add_argument("--engine", default=os.path.join(ROOT, "build", "core", "marketstate"))
    parser.add_argument("--reference", choices=REFERENCES, default="python",
                        help="independent reference model implementation")
    parser.add_argument("--clean", required=True)
    parser.add_argument("--faulty", help="fault-injected stream")
    parser.add_argument("--workdir", default=os.path.join(ROOT, "data", "verify"))
    parser.add_argument("--report-out", help="write JSON report here")
    args = parser.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    engine = os.path.abspath(args.engine)
    report = {"reference": args.reference, "checks": {}, "passed": True}

    # --- 1 & 2: differential + deterministic replay over the clean stream ----
    ref_clean = os.path.join(args.workdir, "ref_clean.state")
    reference_state(args.clean, ref_clean, args.reference)
    ref_text = read(ref_clean)

    eng1 = os.path.join(args.workdir, "engine_clean_1.state")
    eng2 = os.path.join(args.workdir, "engine_clean_2.state")
    engine_replay(engine, args.clean, eng1)
    engine_replay(engine, args.clean, eng2)
    text1 = read(eng1)
    text2 = read(eng2)

    differential = text1 == ref_text
    deterministic = text1 == text2
    report["checks"]["differential_vs_reference"] = {
        "passed": differential,
        "engine_hash": content_hash(text1),
        "reference_hash": content_hash(ref_text),
    }
    report["checks"]["deterministic_replay"] = {
        "passed": deterministic,
        "run1_hash": content_hash(text1),
        "run2_hash": content_hash(text2),
    }

    # --- 3 & 4: chaos + recovery -------------------------------------------
    if args.faulty:
        recovered = os.path.join(args.workdir, "engine_recovered.state")
        engine_run(engine, args.faulty, recovered, recovery_source=args.clean)
        recovered_text = read(recovered)
        recovery_ok = recovered_text == ref_text
        # Unreliable stream with no retransmission source must NOT silently match.
        degraded = os.path.join(args.workdir, "engine_degraded.state")
        engine_run(engine, args.faulty, degraded, recovery_source=None)
        degraded_text = read(degraded)
        report["checks"]["recovery_equivalence"] = {
            "passed": recovery_ok,
            "recovered_hash": content_hash(recovered_text),
            "reference_hash": content_hash(ref_text),
        }
        report["checks"]["unreliable_without_source_differs"] = {
            "passed": degraded_text != ref_text,
            "degraded_hash": content_hash(degraded_text),
        }

    report["passed"] = all(c["passed"] for c in report["checks"].values())
    if args.report_out:
        with open(args.report_out, "w") as handle:
            json.dump(report, handle, indent=2)
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
