#!/usr/bin/env python3
"""Reproducible experiment runner for MarketState.

For each event-count and fault severity it:
  * generates a clean authoritative stream and a fault-injected wire stream,
  * benchmarks the engine on the clean stream (throughput + latency),
  * ingests the faulty stream with a retransmission source and checks that the
    final state equals the reference model of the clean history,
  * exercises checkpoint + replay recovery and checks equivalence,
  * writes a JSON result bundle and a self-contained HTML dashboard.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ENGINE = os.path.join(ROOT, "build", "core", "marketstate")
GENERATOR = os.path.join(ROOT, "tools", "generator", "marketgen.py")
sys.path.insert(0, HERE)
from reflib import REFERENCES, reference_command  # noqa: E402

SEVERITIES = {
    "light": "drop=0.01,dup=0.01,reorder=0.02,corrupt=0.005",
    "medium": "drop=0.03,dup=0.03,reorder=0.05,corrupt=0.02",
    "heavy": "drop=0.08,dup=0.06,reorder=0.10,corrupt=0.05",
}


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def metric_line(stdout):
    for line in reversed(stdout.strip().splitlines()):
        line = line.strip()
        if line.startswith("{"):
            return json.loads(line)
    return {}


def generate(size, seed, out_dir, severity=None):
    clean = os.path.join(out_dir, f"clean_{size}.txt")
    cmd = [sys.executable, GENERATOR, "--events", str(size), "--seed", str(seed),
           "--out", clean, "--symbols", "AAPL,MSFT,GOOG"]
    faulty = None
    scenario = None
    if severity:
        faulty = os.path.join(out_dir, f"faulty_{size}_{severity}.txt")
        scenario = os.path.join(out_dir, f"scenario_{size}_{severity}.json")
        cmd += ["--faulty-out", faulty, "--scenario-out", scenario,
                "--faults", SEVERITIES[severity]]
    run(cmd)
    return clean, faulty, scenario


def bench(size, clean, out_dir):
    result = run([ENGINE, "bench", "--events", clean, "--repeat", "1", "--quiet"])
    metrics = metric_line(result.stdout)
    return {
        "events": size,
        "applied": metrics.get("events_applied", 0),
        "throughput_eps": metrics.get("throughput_eps", 0.0),
        "latency_ns": metrics.get("latency_ns", {}),
    }


def reference_state(clean, out_dir, reference="python"):
    ref = os.path.join(out_dir, "ref.state")
    run(reference_command(reference, clean, ref))
    with open(ref) as handle:
        return handle.read()


def chaos_experiment(size, severity, clean, faulty, out_dir, reference="python"):
    recovered = os.path.join(out_dir, f"recovered_{size}_{severity}.state")
    metrics_out = os.path.join(out_dir, f"metrics_{size}_{severity}.json")
    run([ENGINE, "run", "--events", faulty, "--recovery-source", clean,
         "--state-out", recovered, "--metrics-out", metrics_out, "--quiet"])
    ref_text = reference_state(clean, out_dir, reference)
    with open(recovered) as handle:
        recovered_text = handle.read()
    with open(metrics_out) as handle:
        metrics = json.load(handle)
    return {
        "events": size,
        "severity": severity,
        "equivalent_to_reference": recovered_text == ref_text,
        "metrics": metrics,
    }


def recovery_experiment(size, clean, out_dir):
    db = os.path.join(out_dir, f"log_{size}.sqlite")
    if os.path.exists(db):
        os.remove(db)
    interval = max(1, size // 10)
    run([ENGINE, "run", "--events", clean, "--db", db,
         "--checkpoint-interval", str(interval), "--quiet"])
    rec_out = os.path.join(out_dir, f"recovery_{size}.json")
    run([ENGINE, "recover", "--db", db, "--recovery-out", rec_out])
    with open(rec_out) as handle:
        return json.load(handle)


def html_dashboard(bundle):
    def rows(items, cols):
        out = []
        for item in items:
            tds = "".join(f"<td>{item.get(c, '')}</td>" for c in cols)
            out.append(f"<tr>{tds}</tr>")
        return "\n".join(out)

    bench_cols = ["events", "applied", "throughput_eps", "p50", "p95", "p99", "p999"]
    bench_rows = []
    for b in bundle["benchmarks"]:
        lat = b["latency_ns"]
        bench_rows.append({
            "events": b["events"],
            "applied": b["applied"],
            "throughput_eps": f"{b['throughput_eps']:,.0f}",
            "p50": lat.get("p50", ""),
            "p95": lat.get("p95", ""),
            "p99": lat.get("p99", ""),
            "p999": lat.get("p999", ""),
        })

    chaos_cols = ["events", "severity", "dropped", "duplicated", "corrupted",
                  "duplicates_detected", "events_rejected", "events_recovered",
                  "unrecovered", "equivalent_to_reference"]
    chaos_rows = []
    for c in bundle["chaos"]:
        m = c["metrics"]
        chaos_rows.append({
            "events": c["events"],
            "severity": c["severity"],
            "dropped": c.get("scenario", {}).get("dropped", ""),
            "duplicated": c.get("scenario", {}).get("duplicated", ""),
            "corrupted": c.get("scenario", {}).get("corrupted", ""),
            "duplicates_detected": m.get("duplicates_detected", ""),
            "events_rejected": m.get("events_rejected", ""),
            "events_recovered": m.get("events_recovered", ""),
            "unrecovered": m.get("events_unrecovered", ""),
            "equivalent_to_reference": c["equivalent_to_reference"],
        })

    rec_cols = ["events", "used_checkpoint", "checkpoint_seq", "events_replayed",
                "duration_us", "equivalent"]
    rec_rows = []
    for r in bundle["recovery"]:
        rec_rows.append({
            "events": r.get("events", ""),
            "used_checkpoint": r.get("used_checkpoint", ""),
            "checkpoint_seq": r.get("checkpoint_seq", ""),
            "events_replayed": r.get("events_replayed", ""),
            "duration_us": r.get("duration_us", ""),
            "equivalent": r.get("equivalent", ""),
        })

    return f"""<!doctype html>
<html><head><meta charset="utf-8"><title>MarketState Report</title>
<style>
 body {{ font-family: ui-monospace, Menlo, monospace; background:#0f1117; color:#e6e6e6; margin:2rem; }}
 h1,h2 {{ color:#7dd3fc; }}
 table {{ border-collapse: collapse; margin: 1rem 0 2rem; width:100%; }}
 th,td {{ border:1px solid #2a2f3a; padding:6px 10px; text-align:right; }}
 th {{ background:#1b2230; color:#a5b4fc; }}
 td:first-child, th:first-child {{ text-align:left; }}
 .ok {{ color:#4ade80; font-weight:bold; }}
 .bad {{ color:#f87171; font-weight:bold; }}
 .meta {{ color:#94a3b8; }}
</style></head><body>
<h1>MarketState — Verification &amp; Performance Report</h1>
<p class="meta">Generated {bundle['generated_at']} on {bundle['host']}</p>
<h2>Throughput &amp; latency (clean stream)</h2>
<table><tr>{''.join(f'<th>{c}</th>' for c in bench_cols)}</tr>
{rows(bench_rows, bench_cols)}</table>
<h2>Chaos recovery (faulty stream + retransmission source)</h2>
<table><tr>{''.join(f'<th>{c}</th>' for c in chaos_cols)}</tr>
{rows(chaos_rows, chaos_cols)}</table>
<h2>Checkpoint + replay recovery</h2>
<table><tr>{''.join(f'<th>{c}</th>' for c in rec_cols)}</tr>
{rows(rec_rows, rec_cols)}</table>
</body></html>
"""


def main() -> int:
    parser = argparse.ArgumentParser(description="MarketState experiments")
    parser.add_argument("--sizes", default="10000,100000")
    parser.add_argument("--severities", default="light,medium,heavy")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--reference", choices=REFERENCES, default="python",
                        help="independent reference model implementation")
    parser.add_argument("--workdir", default=os.path.join(ROOT, "data", "experiments"))
    parser.add_argument("--json-out", default=os.path.join(ROOT, "data", "report.json"))
    parser.add_argument("--html-out", default=os.path.join(ROOT, "data", "report.html"))
    args = parser.parse_args()

    sizes = [int(s) for s in args.sizes.split(",") if s.strip()]
    severities = [s.strip() for s in args.severities.split(",") if s.strip()]
    os.makedirs(args.workdir, exist_ok=True)

    bundle = {
        "generated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "host": platform.platform(),
        "python": sys.version.split()[0],
        "reference": args.reference,
        "seed": args.seed,
        "benchmarks": [],
        "chaos": [],
        "recovery": [],
    }

    for size in sizes:
        clean, _, _ = generate(size, args.seed, args.workdir)
        print(f"[bench] {size} events", flush=True)
        bundle["benchmarks"].append(bench(size, clean, args.workdir))

        print(f"[recovery] {size} events", flush=True)
        rec = recovery_experiment(size, clean, args.workdir)
        rec["events"] = size
        bundle["recovery"].append(rec)

        for severity in severities:
            print(f"[chaos] {size} events / {severity}", flush=True)
            _, faulty, scenario_path = generate(size, args.seed, args.workdir, severity)
            with open(scenario_path) as handle:
                scenario = json.load(handle)
            result = chaos_experiment(size, severity, clean, faulty, args.workdir,
                                      args.reference)
            result["scenario"] = scenario
            bundle["chaos"].append(result)

    with open(args.json_out, "w") as handle:
        json.dump(bundle, handle, indent=2)
    with open(args.html_out, "w") as handle:
        handle.write(html_dashboard(bundle))
    print(f"\nreport: {args.json_out}\ndashboard: {args.html_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
