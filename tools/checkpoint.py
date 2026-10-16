#!/usr/bin/env python3
# Builds, measures the codec on corpus/examples, and records a checkpoint.
# usage: tools/checkpoint.py <name> "<description>"    (or with no args to just regenerate the log)

import concurrent.futures, csv, glob, json, math, os, re, shutil, subprocess, sys, tempfile
from collections import defaultdict

import numpy as np

ROOT     = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE     = os.environ.get("CORE", "LiftImage") # the codec repository, cloned inside this one
CP_DIR   = os.path.join(ROOT, "checkpoints")
DEC_SRC  = [f"{CORE}/src/{f}" for f in ("lfi_dec.c", "ent_dec.c", "lfi_internal.h", "lfi.h", "ent.h", "color.h")]
ENC_SRC  = [f"{CORE}/src/{f}" for f in ("lfi_enc.c", "ent_enc.c", "lfi_internal.h", "lfi.h", "ent.h", "color.h")]
DEC_OBJ  = ["build/lfi_dec.o", "build/ent_dec.o"]
ENC_OBJ  = ["build/lfi_enc.o", "build/ent_enc.o"]
CLANG_DIR = "build/clang"
BD_STEPS  = [4, 8, 16, 32]
HDR_STEPS = [8, 16, 32, 64]

def loc(files):
	n = 0
	for f in files:
		for line in open(os.path.join(ROOT, f)):
			s = line.strip()
			if s and not s.startswith("//"): n += 1
	return n

def bin_size(objs):
	out = subprocess.run(["size"] + [os.path.join(ROOT, o) for o in objs], capture_output=True, text=True, check=True).stdout
	return sum(int(l.split()[0]) + int(l.split()[1]) for l in out.splitlines()[1:])

# Only images still in the corpus, so every checkpoint compares over the same set
CORPUS = {os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(ROOT, "corpus", "*", "*"))}

def load_rows(path):
	rows = [r for r in csv.DictReader(open(path)) if r["image"] in CORPUS]
	for r in rows:
		r["codec"] = r["codec"].replace("wav", "lfi") # checkpoints before the rename
		r["gpu"] = r.get("gpu") or r.get("bc1")
		for k in ("param", "w", "h", "bytes", "dec_ms", "enc_ms", "psnr", "psnr_y", "psnr_c", "dark", "ssim", "gpu", "dark_c", "alpha", "angle", "mode", "dec1_ms", "dec2_ms", "bytes1", "bytes2", "prog_ms", "stops", "hdr", "mip2", "ssimulacra2", "butteraugli"): r[k] = float(r.get(k) or "nan")
		r["px"]   = r["w"] * r["h"]
		r["bpp"]  = r["bytes"] * 8 / r["px"]
		r["ssim_db"] = -10 * math.log10(max(1 - r["ssim"], 1e-10))
	return rows

def bd_rate(r1, d1, r2, d2):
	lo, hi = max(min(d1), min(d2)), min(max(d1), max(d2))
	if hi <= lo: return float("nan")
	p1, p2 = np.polyint(np.polyfit(d1, np.log(r1), 3)), np.polyint(np.polyfit(d2, np.log(r2), 3))
	a1 = np.polyval(p1, hi) - np.polyval(p1, lo)
	a2 = np.polyval(p2, hi) - np.polyval(p2, lo)
	return (math.exp((a2 - a1) / (hi - lo)) - 1) * 100

def curves(rows, metric):
	out = defaultdict(dict)
	for r in rows:
		if r["codec"] == "lfi" and r["param"] in BD_STEPS and not math.isnan(r[metric]): out[r["image"]][r["param"]] = (r["bpp"], r[metric])
	return {k: v for k, v in out.items() if len(v) == len(BD_STEPS)}

def mean_bd(base, cur, metric):
	cb, cc = curves(base, metric), curves(cur, metric)
	vals = []
	for img in cb.keys() & cc.keys():
		b = [cb[img][s] for s in BD_STEPS]
		c = [cc[img][s] for s in BD_STEPS]
		v = bd_rate([x[0] for x in b], [x[1] for x in b], [x[0] for x in c], [x[1] for x in c])
		if not math.isnan(v): vals.append(v)
	return sum(vals) / len(vals) if vals else float("nan")

# Mean over a checkpoint's HDR images of BD-rate on metric, each versus the first checkpoint that measured that image with it.
# sign -1 is for metrics where lower is better.
def hdr_bd(cps, cp, metric, sign):
	def curve(c, img):
		rows = sorted((r for r in c["hdr"] if r["image"] == img and r["codec"] == "lfi" and r["param"] in HDR_STEPS), key=lambda r: r["param"])
		return rows if len(rows) == len(HDR_STEPS) and not any(math.isnan(r[metric]) for r in rows) else None
	vals = []
	for img in {r["image"] for r in cp["hdr"]}:
		cur, base = curve(cp, img), next((b for b in (curve(c, img) for c in cps) if b), None)
		if cur and base: vals.append(bd_rate([r["bpp"] for r in base], [sign * r[metric] for r in base], [r["bpp"] for r in cur], [sign * r[metric] for r in cur]))
	vals = [v for v in vals if not math.isnan(v)]
	return sum(vals) / len(vals) if vals else float("nan")

def aggregate(rows, codec, param):
	sel = [r for r in rows if r["codec"] == codec and r["param"] == param]
	px  = sum(r["px"] for r in sel)
	def avg(k):
		v = [r[k] for r in sel if not math.isnan(r[k])]
		return sum(v) / len(v) if v else float("nan")
	return {
		"bpp": sum(r["bytes"] * 8 for r in sel) / px,
		"dec": px / sum(r["dec_ms"] for r in sel) / 1e3,
		"enc": px / sum(r["enc_ms"] for r in sel) / 1e3 if sum(r["enc_ms"] for r in sel) else float("nan"),
		**{k: avg(k) for k in ("psnr", "psnr_y", "psnr_c", "dark", "dark_c", "ssim", "gpu", "ssimulacra2", "butteraugli")},
	}

def checkpoints():
	cps = []
	for meta in sorted(glob.glob(os.path.join(CP_DIR, "*.json"))):
		m = json.load(open(meta))
		m["rows"] = load_rows(meta[:-5] + ".csv")
		if os.path.exists(meta[:-5] + ".modes.csv"): m["modes"] = load_rows(meta[:-5] + ".modes.csv")
		if os.path.exists(meta[:-5] + ".hdr.csv"):   m["hdr"]   = load_rows(meta[:-5] + ".hdr.csv")
		if os.path.exists(meta[:-5] + ".clang.csv"): m["clang"] = load_rows(meta[:-5] + ".clang.csv")
		cps.append(m)
	return cps

def fmt(v, f="{:.2f}"):
	return "n/a" if v is None or (isinstance(v, float) and math.isnan(v)) else f.format(v)

# Interpolates aggregate results to a target PSNR between the two steps that bracket it
def at_psnr(rows, target):
	pts = sorted((aggregate(rows, "lfi", s) for s in BD_STEPS), key=lambda a: a["psnr"])
	for lo, hi in zip(pts, pts[1:]):
		if lo["psnr"] <= target <= hi["psnr"]:
			t = (target - lo["psnr"]) / (hi["psnr"] - lo["psnr"])
			return {k: lo[k] + (hi[k] - lo[k]) * t for k in ("bpp", "dec", "enc", "gpu", "dark_c")}
	return {k: float("nan") for k in ("bpp", "dec", "enc", "gpu", "dark_c")}

# Each metric is compared against the first checkpoint that measured it
def bd_vs_first(cps, cp, metric):
	base = next((c for c in cps if curves(c["rows"], metric)), None)
	return mean_bd(base["rows"], cp["rows"], metric) if base else float("nan")

# Each modes image is judged by the metric that matters for it
MODE_METRIC = {1: "psnr", 2: "angle", 3: "psnr", 4: "psnr"}

def modes_bd(cps, cp, img):
	have = [c for c in cps if c.get("modes") and any(r["image"] == img for r in c["modes"])]
	if not have or not cp.get("modes"): return float("nan")
	rows = [r for r in cp["modes"] if r["image"] == img]
	base = [r for r in have[0]["modes"] if r["image"] == img]
	if not rows: return float("nan")
	metric = MODE_METRIC[int(rows[0]["mode"])]
	if metric == "angle":
		for r in rows + base: r["neg_angle"] = -r["angle"]
		metric = "neg_angle"
	return mean_bd(base, rows, metric)

def write_log(cps):
	images = {r["image"]: r["px"] for r in cps[-1]["rows"]} if cps else {}
	lines = ["# Checkpoints", "",
		f"Measured on `corpus/examples` ({len(images)} images, {sum(images.values()) / 1e6:.1f} MP), single thread. Speeds are best-of-N wall time over two runs, CPU fixed near 2 GHz (powersave), noise around 3%.",
		"@37dB columns interpolate between lfi steps to equal average PSNR, so checkpoints compare at matched quality.",
		"BD-rate is the average file size change at equal quality versus the first checkpoint with that metric, over lfi steps 4 to 32 (negative is smaller).",
		"Dark is PSNR over pixels with source luma < 64, relative to that range; dark C is the chroma part of it. LOC excludes blank and comment lines; shared headers count for both.",
		"S2 is SSIMULACRA2 and BA is 3-norm Butteraugli (libjxl devtools), scored on the color images only.",
		"Binary sizes and speeds are gcc builds, with clang's alongside from checkpoint 11, both at x86-64-v3.",
		"",
		"| # | change | dec LOC | enc LOC | dec bytes | dec bytes clang | enc bytes | bpp @37dB | dec MP/s @37dB | dec MP/s clang | enc MP/s @37dB | BC1 @37dB | dark C @37dB | BD PSNR | BD BC1 | BD SSIM | BD dark | BD dark C | BD S2 | BD BA | lossless bpp |",
		"|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
	for cp in cps:
		m, s0 = at_psnr(cp["rows"], 37), aggregate(cp["rows"], "lfi", 0)
		bd = [bd_vs_first(cps, cp, m) for m in ("psnr", "gpu", "ssim_db", "dark", "dark_c", "ssimulacra2", "butteraugli")]
		mc = at_psnr(cp["clang"], 37) if cp.get("clang") else {"dec": float("nan")}
		lines.append(f"| {cp['id']} | {cp['desc']} | {cp['loc_dec']} | {cp['loc_enc']} | {cp['bin_dec']} | {cp.get('bin_dec_clang', 'n/a')} | {cp['bin_enc']} | "
			f"{fmt(m['bpp'], '{:.3f}')} | {fmt(m['dec'], '{:.0f}')} | {fmt(mc['dec'], '{:.0f}')} | {fmt(m['enc'], '{:.1f}')} | {fmt(m['gpu'])} | {fmt(m['dark_c'])} | "
			+ " | ".join(fmt(v, "{:+.1f}%") for v in bd) + f" | {fmt(s0['bpp'], '{:.3f}')} |")

	prog_cps = [c for c in cps if any(not math.isnan(r["dec1_ms"]) for r in c["rows"])]
	if prog_cps:
		lines += ["", "## Progressive", "",
			"Main set at step 16: decode speedup of mip 1 and mip 2 over a full decode, the share of the file each needs,",
			"and the cost of decoding mip 2, then 1, then 0 with each step continuing from the last, relative to one full decode.", "",
			"| # | change | mip 1 speedup | mip 2 speedup | mip 1 prefix | mip 2 prefix | 2 > 1 > 0 cost |",
			"|---|---|---|---|---|---|---|"]
		for cp in prog_cps:
			sel   = [r for r in cp["rows"] if r["codec"] == "lfi" and r["param"] == 16]
			full  = sum(r["dec_ms"] for r in sel)
			bytes = sum(r["bytes"] for r in sel)
			lines.append(f"| {cp['id']} | {cp['desc']} | {full / sum(r['dec1_ms'] for r in sel):.1f}x | {full / sum(r['dec2_ms'] for r in sel):.1f}x | "
				f"{100 * sum(r['bytes1'] for r in sel) / bytes:.0f}% | {100 * sum(r['bytes2'] for r in sel) / bytes:.0f}% | "
				f"{fmt(sum(r['prog_ms'] for r in sel) / full, '{:.2f}x')} |")

	mode_cps = [c for c in cps if c.get("modes")]
	if mode_cps:
		images = sorted({r["image"] for r in mode_cps[-1]["modes"]})
		lines += ["", "## Modes", "",
			"`corpus/modes`: gray (1 channel, judged by PSNR), normal XY (2 channels, judged by angle error), color + alpha (judged by PSNR on visible pixels).",
			"Alpha images: fern (soft, dilated color), cutout (0/255), forcecut (soft alpha coded as cutout), premul (premultiplied), blackbg (black under transparent pixels).",
			"mip 2 is premultiplied PSNR of the mip 2 decode against a 4x4 box downsample, where hidden colors show as fringes.",
			"BD-rate per image versus the first checkpoint with that image. dec MP/s is over the whole set at step 16.", "",
			"| # | change | dec MP/s @16 | " + " | ".join(f"BD {i}" for i in images) + " |",
			"|---|---|---|" + "---|" * len(images)]
		for cp in mode_cps:
			a = aggregate(cp["modes"], "lfi", 16)
			lines.append(f"| {cp['id']} | {cp['desc']} | {fmt(a['dec'], '{:.0f}')} | " + " | ".join(fmt(modes_bd(cps, cp, i), "{:+.1f}%") for i in images) + " |")

	hdr_cps = [c for c in cps if c.get("hdr")]
	if hdr_cps:
		lines += ["", "## HDR", "",
			"`corpus/hdr`: 10 Poly Haven HDRIs at 1024x512. Tone mapped PSNR and SSIMULACRA2 are each averaged over",
			"exposures 2 stops either side of the log average, and stops is RMS error in stops. Steps are in 1/128 stop. BD-rates are over steps 8 to 64,",
			"averaged over images, each versus the first checkpoint with that image and metric.", "",
			"| # | change | images | src bpp | lfi16 bpp | dec MP/s @16 | BD tone mapped PSNR | BD SSIMULACRA2 | BD stops |",
			"|---|---|---|---|---|---|---|---|---|"]
		for cp in hdr_cps:
			src, w16 = aggregate(cp["hdr"], "src", 0), aggregate(cp["hdr"], "lfi", 16)
			lines.append(f"| {cp['id']} | {cp['desc']} | {len({r['image'] for r in cp['hdr']})} | {src['bpp']:.2f} | {w16['bpp']:.3f} | {fmt(w16['dec'], '{:.0f}')} | " +
				" | ".join(fmt(hdr_bd(hdr_cps, cp, m, sign), "{:+.1f}%") for m, sign in (("psnr", 1), ("ssimulacra2", 1), ("stops", -1))) + " |")

	fmt_cps = [c for c in hdr_cps if any(r["codec"] == "lfi11" for r in c["hdr"])]
	if fmt_cps:
		lines += ["", "### HDR output formats at step 16", "",
			"Decode MP/s and mean tone mapped PSNR over the images, for each output format.", "",
			"| # | RGBA16F | RG11B10F | RGB9E5 |", "|---|---|---|---|"]
		for cp in fmt_cps:
			cells = [aggregate(cp["hdr"], tag, 16) for tag in ("lfi", "lfi11", "lfi9e5")]
			lines.append(f"| {cp['id']} | " + " | ".join(f"{fmt(a['dec'], '{:.0f}')} MP/s, {a['psnr']:.2f} dB" for a in cells) + " |")

	for cp in reversed(cps):
		if cp.get("modes"):
			lines += ["", f"## {cp['id']} modes", "",
				"| image | ch | step | bpp | PSNR | GPU | alpha | angle | mip 2 | dec MP/s |",
				"|---|---|---|---|---|---|---|---|---|---|"]
			for r in sorted((r for r in cp["modes"] if r["codec"] == "lfi"), key=lambda r: (r["image"], r["param"])):
				lines.append(f"| {r['image']} | {r['mode']:.0f} | {r['param']:g} | {r['bpp']:.3f} | {fmt(r['psnr'])} | {fmt(r['gpu'])} | "
					f"{fmt(r['alpha'])} | {fmt(r['angle'])} | {fmt(r['mip2'])} | {r['px'] / r['dec_ms'] / 1e3:.0f} |")
		lines += ["", f"## {cp['id']}: {cp['desc']}", "",
			"| config | bpp | dec MP/s | enc MP/s | PSNR | Y | C | dark | dark C | SSIM | BC1 | S2 | BA |",
			"|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
		for codec, param in [("jpg", 75), ("jpg", 90), ("lfi", 0)] + [("lfi", s) for s in BD_STEPS]:
			a = aggregate(cp["rows"], codec, param)
			lines.append(f"| {codec}{param:g} | {a['bpp']:.3f} | {a['dec']:.0f} | {fmt(a['enc'], '{:.1f}')} | {fmt(a['psnr'])} | {fmt(a['psnr_y'])} | "
				f"{fmt(a['psnr_c'])} | {fmt(a['dark'])} | {fmt(a['dark_c'])} | {fmt(a['ssim'], '{:.4f}')} | {fmt(a['gpu'])} | {fmt(a['ssimulacra2'])} | {fmt(a['butteraugli'], '{:.3f}')} |")
	open(os.path.join(CP_DIR, "log.md"), "w").write("\n".join(lines) + "\n")
	print("\n".join(lines[8:10 + len(cps)]))

# Scorers may use every core, even when the benchmark is pinned with taskset
def unpin():
	if hasattr(os, "sched_setaffinity"): os.sched_setaffinity(0, range(os.cpu_count() or 1))

def perceptual(reference, test):
	s2  = subprocess.run(["ssimulacra2", reference, test], capture_output=True, text=True, check=True, preexec_fn=unpin).stdout.split()[-1]
	out = subprocess.run(["butteraugli_main", reference, test, "--pnorm", "3"], capture_output=True, text=True, check=True, preexec_fn=unpin).stdout
	return s2, next(l.split()[-1] for l in out.splitlines() if "norm" in l)

# SSIMULACRA2 and 3-norm Butteraugli of the color rows, from libjxl's devtools
def score(rows, corpus, dump):
	sources = {os.path.splitext(os.path.basename(p))[0]: p for p in glob.glob(os.path.join(ROOT, corpus, "*"))}
	jobs    = {}
	with concurrent.futures.ThreadPoolExecutor(max(1, (os.cpu_count() or 2) - 1)) as pool:
		for i, r in enumerate(rows):
			if r["mode"] == "3" and (r["codec"] == "jpg" or (r["codec"] == "lfi" and float(r["param"]) in BD_STEPS)):
				jobs[i] = pool.submit(perceptual, sources[r["image"]], os.path.join(dump, f"{r['image']}_{r['codec']}{float(r['param']):g}.png"))
	for i, r in enumerate(rows):
		r["ssimulacra2"], r["butteraugli"] = jobs[i].result() if i in jobs else ("nan", "nan")

# SSIMULACRA2 of the HDR rows, averaged over the bench's renders 2 stops either side of the log average
def score_hdr(rows, dump):
	def s2(reference, test):
		return float(subprocess.run(["ssimulacra2", reference, test], capture_output=True, text=True, check=True, preexec_fn=unpin).stdout.split()[-1])
	jobs = {}
	with concurrent.futures.ThreadPoolExecutor(max(1, (os.cpu_count() or 2) - 1)) as pool:
		for i, r in enumerate(rows):
			if r["codec"] == "lfi" and float(r["param"]) in HDR_STEPS:
				test = f"{r['image']}_lfi{float(r['param']):g}"
				jobs[i] = [pool.submit(s2, os.path.join(dump, f"{r['image']}_src_ev{e:+d}.png"), os.path.join(dump, f"{test}_ev{e:+d}.png")) for e in (-2, 0, 2)]
	for i, r in enumerate(rows):
		r["ssimulacra2"], r["butteraugli"] = (str(sum(j.result() for j in jobs[i]) / 3), "nan") if i in jobs else ("nan", "nan")

# The bench again with clang, the compiler that matters most, into CLANG_DIR. gcc vectorizes some loops clang doesn't,
# and the reverse, so the gcc build alone has hidden clang slowdowns.
def build_clang():
	os.makedirs(os.path.join(ROOT, CLANG_DIR), exist_ok=True)
	turbo = subprocess.run(["pkg-config", "--exists", "libturbojpeg"]).returncode == 0
	pkg   = lambda flag: subprocess.run(["pkg-config", flag, "libturbojpeg"], capture_output=True, text=True).stdout.split() if turbo else []
	flags = ["-std=c11", "-O3", "-g", "-march=x86-64-v3", "-Wall", "-Wextra", "-Wno-unused-function", f"-I{CORE}/src", f"-I{CORE}/third_party", "-Ithird_party"]
	objs  = []
	for src in (f"{CORE}/src/ent_enc.c", f"{CORE}/src/ent_dec.c", f"{CORE}/src/lfi_enc.c", f"{CORE}/src/lfi_dec.c", "bench/bench.c"):
		obj = os.path.join(CLANG_DIR, os.path.basename(src)[:-2] + ".o")
		defs = ["-DBENCH_TURBOJPEG"] + pkg("--cflags") if turbo and src.endswith("bench.c") else []
		subprocess.run(["clang"] + flags + defs + ["-c", "-o", obj, src], cwd=ROOT, check=True, capture_output=True)
		objs.append(obj)
	subprocess.run(["clang", "-o", os.path.join(CLANG_DIR, "bench")] + objs + ["build/stb_impl.o"] + pkg("--libs") + ["-lm"], cwd=ROOT, check=True, capture_output=True)

# Best of two bench runs per row, since other load on the machine skews single runs. The first dumps for perceptual
# scores, unless just timing another build.
def measure(out_csv, corpus="corpus/examples", bench="build/bench"):
	runs = []
	hdr  = corpus == "corpus/hdr"
	dump = tempfile.mkdtemp() if bench == "build/bench" and (corpus == "corpus/examples" or hdr) and shutil.which("ssimulacra2") and (hdr or shutil.which("butteraugli_main")) else None
	for i in range(2):
		path = f"{out_csv}.{i}"
		subprocess.run([os.path.join(ROOT, bench), "-c", path] + (["-d", dump] if dump and i == 0 else []) + [os.path.join(ROOT, corpus)], cwd=ROOT, check=True, capture_output=True)
		runs.append(list(csv.DictReader(open(path))))
		os.remove(path)
	for row, other in zip(runs[0], runs[1]):
		for k in ("dec_ms", "enc_ms", "dec1_ms", "dec2_ms", "prog_ms"): row[k] = str(min(float(row[k]), float(other[k])))
	if dump:
		if hdr: score_hdr(runs[0], dump)
		else:   score(runs[0], corpus, dump)
		shutil.rmtree(dump)
	with open(out_csv, "w", newline="") as f:
		w = csv.DictWriter(f, fieldnames=runs[0][0].keys())
		w.writeheader()
		w.writerows(runs[0])

# Times corpus/examples with the clang build into the checkpoint's .clang.csv, and adds clang's binary sizes to its json
def measure_clang(stem):
	build_clang()
	measure(stem + ".clang.csv", bench=os.path.join(CLANG_DIR, "bench"))
	meta = json.load(open(stem + ".json")) if os.path.exists(stem + ".json") else {}
	meta["bin_dec_clang"] = bin_size([os.path.join(CLANG_DIR, o) for o in ("lfi_dec.o", "ent_dec.o")])
	meta["bin_enc_clang"] = bin_size([os.path.join(CLANG_DIR, o) for o in ("lfi_enc.o", "ent_enc.o")])
	return meta

# The codec commit measured, marked when it has uncommitted changes, or None outside git
def core_version():
	run = lambda *args: subprocess.run(["git", "-C", os.path.join(ROOT, CORE), *args], capture_output=True, text=True)
	commit = run("rev-parse", "--short", "HEAD")
	if commit.returncode: return None
	return commit.stdout.strip() + ("+" if run("status", "--porcelain").stdout.strip() else "")

def main():
	if len(sys.argv) == 3 and sys.argv[1] == "--remeasure":
		measure(sys.argv[2])
		write_log(checkpoints())
		return
	# --clang <checkpoint stem> times the current source's clang build for an existing checkpoint
	if len(sys.argv) == 3 and sys.argv[1] == "--clang":
		json.dump(measure_clang(sys.argv[2]), open(sys.argv[2] + ".json", "w"), indent=1)
		write_log(checkpoints())
		return
	os.makedirs(CP_DIR, exist_ok=True)
	if len(sys.argv) >= 3:
		name, desc = sys.argv[1], sys.argv[2]
		subprocess.run(["make", "-j8"], cwd=ROOT, check=True, capture_output=True)
		cp_id = f"{len(glob.glob(os.path.join(CP_DIR, '*.json'))):02d}"
		stem  = os.path.join(CP_DIR, f"{cp_id}_{re.sub(r'[^a-z0-9]+', '_', name.lower())}")
		measure(stem + ".csv")
		measure(stem + ".modes.csv", "corpus/modes")
		measure(stem + ".hdr.csv", "corpus/hdr")
		meta = {"id": cp_id, "desc": desc, "core": core_version(), "loc_dec": loc(DEC_SRC), "loc_enc": loc(ENC_SRC), "bin_dec": bin_size(DEC_OBJ), "bin_enc": bin_size(ENC_OBJ)}
		json.dump({**meta, **measure_clang(stem)}, open(stem + ".json", "w"), indent=1)
	write_log(checkpoints())

if __name__ == "__main__": main()
