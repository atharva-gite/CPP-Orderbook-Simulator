#!/usr/bin/env python3
"""SVG snapshots -> PNG via qlmanage -> docs/assets/terminal.gif"""
from __future__ import annotations

import html
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "demo" / "assets"
OUT = ROOT / "docs" / "assets"
OUT.mkdir(parents=True, exist_ok=True)


def strip(s: str) -> str:
    return re.sub(r"\x1b\[[0-9;]*m", "", s)


def svg_for(title: str, body: str) -> str:
    lines = [title, ""] + body.splitlines()
    ts = []
    y = 40
    for ln in lines[:28]:
        ts.append(f'<text x="32" y="{y}" fill="#c8d1d9">{html.escape(ln)}</text>')
        y += 22
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="720">
  <rect width="100%" height="100%" fill="#0d1117"/>
  <rect x="16" y="16" width="1248" height="688" rx="8" fill="#161b22" stroke="#30363d"/>
  <g font-family="Menlo, ui-monospace, monospace" font-size="15">{"".join(ts)}</g>
</svg>
'''


md = strip((ASSETS / "md_tests.txt").read_text(errors="replace"))
bench = strip((ASSETS / "bench_snip.txt").read_text(errors="replace"))
md_body = "\n".join(ln.rstrip() for ln in md.splitlines() if ln.strip())[-1800:]
keep = []
on = False
for ln in bench.splitlines():
    if ln.startswith("Clock:") or ln.startswith("op ") or ln.startswith("add_") or ln.startswith("cancel") or ln.startswith("market") or ln.startswith("sim_") or ln.startswith("ANOMALY") or ln.startswith("----"):
        on = True
    if on:
        keep.append(ln.rstrip())
bench_body = "\n".join(keep[:20])

tmpdir = Path(tempfile.mkdtemp(prefix="lobgif-"))
pngs = []
for i, (title, body) in enumerate(
    [
        ("$ ./build/lob_md_tests [md]", md_body),
        ("$ ./build/lob_bench  (pool, depth 10/100)", bench_body),
    ]
):
    svgp = tmpdir / f"s{i}.svg"
    svgp.write_text(svg_for(title, body))
    subprocess.check_call(
        ["qlmanage", "-t", "-s", "1280", "-o", str(tmpdir), str(svgp)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    # qlmanage names file s0.svg.png
    cand = list(tmpdir.glob(f"s{i}*png"))
    if not cand:
        raise SystemExit(f"qlmanage produced no png for scene {i}: {list(tmpdir.iterdir())}")
    pngs.append(cand[0])

lst = tmpdir / "list.txt"
parts = []
for p in pngs:
    parts.append(f"file '{p}'\nduration 2.8\n")
parts.append(f"file '{pngs[-1]}'\n")
lst.write_text("".join(parts))
gif = OUT / "terminal.gif"
subprocess.check_call(
    [
        "ffmpeg", "-y", "-f", "concat", "-safe", "0", "-i", str(lst),
        "-vf", "fps=5,scale=1280:-1:flags=lanczos,split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse",
        "-loop", "0",
        str(gif),
    ],
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
print("wrote", gif, gif.stat().st_size)
