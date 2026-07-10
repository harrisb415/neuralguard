#!/usr/bin/env python3
"""
Off-device trainer for the NeuralGuard Phase-4 *anomaly* scorer (ROADMAP 4b).

Reads completed-flow features from a ngpolicy.db (the `flow_features` table that
`ngd features` populates), trains an unsupervised Isolation Forest on *your own*
traffic - it learns what your normal looks like - and exports it to ONNX so the
engine can score finished flows on-device, in shadow mode, off the decision path.

This runs on your DEV machine, never on the engine. It reads only your local DB;
nothing leaves the machine. The model is only as good as the data behind it: run
`ngd features on` and use the machine normally for a while first. A model trained
on a few dozen flows is a placeholder, not a detector - that's expected early on.

Usage:
    python scripts/train_anomaly.py --db ngpolicy.db --out models/anomaly.onnx

Requires: numpy, scikit-learn, skl2onnx, onnx   (see scripts/requirements-ml.txt)

The feature order below is the contract between this trainer and the on-device
scorer: whatever ngd builds per flow must match FEATURE_NAMES exactly. It's
written to <out>.json alongside the model so the two can't silently drift.
"""
import argparse
import json
import math
import sqlite3
import sys
from pathlib import Path

# The feature vector, in order. Keep in lockstep with the ngd-side builder.
FEATURE_NAMES = [
    "log_duration",   # log1p(duration_ms)
    "log_bytes_in",   # log1p(bytes_in)
    "log_bytes_out",  # log1p(bytes_out)
    "out_ratio",      # bytes_out / (bytes_in + bytes_out + 1)  - exfil signal
    "is_https",       # remote_port == 443
    "is_http",        # remote_port == 80
    "is_signed",      # process is Authenticode-signed (key starts "sig:")
    "hour",           # UTC hour-of-day of completion (0-23)
]


def featurize(row):
    ts, pkey, _dest, port, dur, bin_, bout = row
    bin_ = bin_ or 0
    bout = bout or 0
    total = bin_ + bout
    hour = int(ts[11:13]) if ts and len(ts) >= 13 and ts[11:13].isdigit() else 0
    return [
        math.log1p(dur or 0),
        math.log1p(bin_),
        math.log1p(bout),
        bout / (total + 1.0),
        1.0 if (port or 0) == 443 else 0.0,
        1.0 if (port or 0) == 80 else 0.0,
        1.0 if (pkey or "").startswith("sig:") else 0.0,
        float(hour),
    ]


def load_flows(db_path):
    con = sqlite3.connect(db_path)
    try:
        rows = con.execute(
            "SELECT ts_utc, process_key, dest, remote_port, duration_ms, bytes_in, bytes_out "
            "FROM flow_features"
        ).fetchall()
    finally:
        con.close()
    return rows


def main():
    ap = argparse.ArgumentParser(description="Train the NeuralGuard anomaly scorer (Isolation Forest -> ONNX).")
    ap.add_argument("--db", required=True, help="path to ngpolicy.db")
    ap.add_argument("--out", default="models/anomaly.onnx", help="output ONNX model path")
    ap.add_argument("--contamination", default="auto",
                    help="'auto' (default) or a float in (0, 0.5] - expected anomaly fraction")
    ap.add_argument("--estimators", type=int, default=200)
    ap.add_argument("--min-rows", type=int, default=200,
                    help="warn below this many rows (still trains if >0)")
    args = ap.parse_args()

    import numpy as np
    from sklearn.ensemble import IsolationForest
    from skl2onnx import to_onnx

    rows = load_flows(args.db)
    if not rows:
        sys.exit("no flow_features rows found - run `ngd features on` and use the machine, then retrain.")
    if len(rows) < args.min_rows:
        print(f"WARNING: only {len(rows)} rows (< {args.min_rows}). Training a placeholder model; "
              f"let more data accumulate for a meaningful one.", file=sys.stderr)

    X = np.array([featurize(r) for r in rows], dtype=np.float32)
    contamination = args.contamination if args.contamination == "auto" else float(args.contamination)
    clf = IsolationForest(n_estimators=args.estimators, contamination=contamination, random_state=0)
    clf.fit(X)

    # Export. The ONNX graph outputs the same score_samples/decision the engine
    # thresholds; lower = more anomalous. Pin the opsets: skl2onnx can lag the
    # onnx package's newest 'ai.onnx.ml' domain version, so cap it at 3 (widely
    # supported by runtimes) rather than letting it pick an unsupported v4+.
    onx = to_onnx(clf, X[:1].astype(np.float32), target_opset={"": 17, "ai.onnx.ml": 3})

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(onx.SerializeToString())

    spec = {
        "model": "IsolationForest",
        "features": FEATURE_NAMES,
        "n_features": len(FEATURE_NAMES),
        "n_rows_trained": len(rows),
        "estimators": args.estimators,
        "contamination": str(contamination),
        "note": "feature order is the contract with the ngd-side vector builder",
    }
    out.with_suffix(".json").write_text(json.dumps(spec, indent=2))

    print(f"trained on {len(rows)} flows -> {out} ({out.stat().st_size} bytes)")
    print(f"feature spec  -> {out.with_suffix('.json')}")


if __name__ == "__main__":
    main()
