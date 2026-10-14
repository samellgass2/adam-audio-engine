#!/usr/bin/env python3
"""Compare two renders of the same material (stdlib only).

Aligns them (best lag within +-max_lag samples, so a host's reported latency
does not count as a difference), then reports the max sample difference and
the residual energy relative to the reference, in dB. Exit 0 when the
residual is below --threshold-db (default -80 dB: inaudible).

    python3 tools/compare_wav.py out/reference.wav out/candidate.wav
"""
from __future__ import annotations

import argparse
import json
import math
import struct
import sys


def read_wav(path: str) -> tuple[int, int, list[float]]:
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit(f"{path}: not a WAV")
    pos, fmt, samples = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, sr, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE: real tag in the subformat GUID
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, ch, sr, bits)
        elif cid == b"data":
            samples = body
        pos += 8 + size + (size & 1)
    tag, ch, sr, bits = fmt
    if tag == 3 and bits == 32:
        vals = list(struct.unpack(f"<{len(samples) // 4}f", samples))
    elif tag == 1 and bits == 16:
        vals = [v / 32768 for v in struct.unpack(f"<{len(samples) // 2}h", samples)]
    else:
        sys.exit(f"{path}: unsupported format tag={tag} bits={bits}")
    return sr, ch, vals


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--max-lag", type=int, default=256)
    ap.add_argument("--threshold-db", type=float, default=-80.0)
    a = ap.parse_args()
    sr1, ch1, x = read_wav(a.reference)
    sr2, ch2, y = read_wav(a.candidate)
    if (sr1, ch1) != (sr2, ch2):
        sys.exit(f"format mismatch: {sr1}/{ch1} vs {sr2}/{ch2}")
    left_x, left_y = x[0::ch1], y[0::ch2]
    n = min(len(left_x), len(left_y)) - a.max_lag

    def err(lag: int, step: int = 7) -> float:
        return sum((left_x[i] - left_y[i + lag]) ** 2 for i in range(max(0, -lag), n, step))

    lag = min(range(-a.max_lag, a.max_lag + 1), key=err)
    max_diff = ref = res = 0.0
    for c in range(ch1):
        xs, ys = x[c::ch1], y[c::ch2]
        for i in range(max(0, -lag), n):
            d = xs[i] - ys[i + lag]
            max_diff = max(max_diff, abs(d))
            ref += xs[i] * xs[i]
            res += d * d
    residual_db = 10 * math.log10(res / ref + 1e-30) if ref > 0 else 0.0
    report = {"lag_samples": lag, "max_abs_diff": max_diff, "residual_db": round(residual_db, 1),
              "frames_compared": n, "ok": residual_db < a.threshold_db}
    print(json.dumps(report))
    sys.exit(0 if report["ok"] else 1)


if __name__ == "__main__":
    main()
