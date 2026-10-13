#!/usr/bin/env python3
"""Assemble the static workshop: dist/workshop/ = workshop/src + adam_synth.wasm
+ the fixture patterns and presets (with index.json) + build.json. Every URL in
the app is relative, so the folder works at any path (e.g. /staging/<port>/)."""
from __future__ import annotations

import json
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "dist" / "workshop"


def main() -> None:
    if OUT.exists():
        shutil.rmtree(OUT)
    shutil.copytree(ROOT / "workshop" / "src", OUT)
    shutil.copy2(ROOT / "build" / "wasm" / "adam_synth.wasm", OUT / "adam_synth.wasm")
    shutil.copytree(ROOT / "fixtures" / "patterns", OUT / "patterns")
    (OUT / "presets").mkdir()
    presets = []
    order = ["init", "soft_pad", "bright_chords", "pluck"]
    files = sorted((ROOT / "fixtures" / "presets").glob("*.preset"),
                   key=lambda p: (order.index(p.stem) if p.stem in order else 99, p.stem))
    for f in files:
        shutil.copy2(f, OUT / "presets" / f.name)
        first = next((ln[1:].strip() for ln in f.read_text().splitlines() if ln.startswith("#")), "")
        name = first.split(":")[0] if first else f.stem.replace("_", " ").title()
        presets.append({"file": f.name, "name": name, "description": first})
    (OUT / "presets" / "index.json").write_text(json.dumps(presets, indent=2) + "\n")
    try:
        commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                                capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT,
                               capture_output=True, text=True).stdout.strip()
        commit += "+dirty" if dirty else ""
    except Exception:
        commit = "uncommitted"
    built = datetime.now(timezone.utc).astimezone().strftime("%Y-%m-%d %H:%M")
    (OUT / "build.json").write_text(json.dumps({"commit": commit, "built": built}) + "\n")
    print(f"workshop -> {OUT} ({commit}, {len(presets)} presets)")


if __name__ == "__main__":
    main()
