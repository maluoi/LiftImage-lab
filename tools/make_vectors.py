#!/usr/bin/env python3
# Re-encodes the codec's test vectors from tests/vectors, then writes their hashes into the table in its docs/format.md.
# Run after a format change, then check the codec with make -C LiftImage test.
# usage: tools/make_vectors.py      (after make lfi)
import os, re, subprocess

ROOT    = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE    = os.path.join(ROOT, os.environ.get("CORE", "LiftImage"))
LFI     = os.path.join(CORE, "build", "lfi")
SOURCES = os.path.join(ROOT, "tests", "vectors")
VECTORS = os.path.join(CORE, "docs", "vectors")

ENCODES = {
	"rgb16":     ["--step", "16"],
	"rgba8":     ["--step", "8", "--alpha", "blend"],
	"lossless":  ["--step", "0", "--alpha", "blend"],
	"gray32":    ["--step", "32", "--channels", "1"],
	"normal8":   ["--step", "8", "--channels", "2"],
	"cutout16":  ["--step", "16", "--alpha", "cutout"],
	"premul16":  ["--step", "16", "--alpha", "blend", "--premultiplied", "--dark", "4"],
	"tiles32":   ["--step", "16", "--alpha", "blend", "--tile", "5"],
	"array8":    ["--step", "8", "--alpha", "blend"],
	"cube16":    ["--step", "16", "--alpha", "blend", "--cubemap"],
	"hdr16":     ["--step", "16"],
	"hdrgray16": ["--step", "16", "--channels", "1"],
	"data8":     ["--step", "8", "--data"],
}
LAYERS = {"array8": 3, "cube16": 6}

def lfi(*args): return subprocess.run([LFI, *args], capture_output=True, text=True, check=True).stdout.strip()

hashes = {}
for name, options in ENCODES.items():
	extension = ".hdr" if name.startswith("hdr") else ".png"
	inputs    = [f"{name}_{i}{extension}" for i in range(LAYERS[name])] if name in LAYERS else [name + extension]
	path      = os.path.join(VECTORS, name + ".lfi")
	lfi("encode", *options, path, *[os.path.join(SOURCES, i) for i in inputs])
	hashes[name + ".lfi"] = {
		"mip0": lfi("hash", path), "mip1": lfi("hash", "--mip", "1", path),
		**({"rg11b10f": lfi("hash", "--format", "rg11b10f", path), "rgb9e5": lfi("hash", "--format", "rgb9e5", path)} if name.startswith("hdr") else {}),
	}

spec  = os.path.join(CORE, "docs", "format.md")
lines = open(spec).read().split("\n")
for i, line in enumerate(lines):
	row = re.match(r"\| `(\w+\.lfi)` \| ([^|]*) \| `\w+` \| `\w+` \| (.*) \|$", line)
	if not row: continue
	h     = hashes[row.group(1)]
	other = f"RG11B10F `{h['rg11b10f']}`, RGB9E5 `{h['rgb9e5']}`" if "rgb9e5" in h else row.group(3)
	lines[i] = f"| `{row.group(1)}` | {row.group(2)} | `{h['mip0']}` | `{h['mip1']}` | {other} |"
open(spec, "w").write("\n".join(lines))
print(f"wrote {len(hashes)} vectors and their hashes")
