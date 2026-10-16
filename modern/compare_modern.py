#!/usr/bin/env python3
# compare.py extended to WebP, JPEG XL, AVIF and jpegli, on build/compare_modern built from ../LiftImage.
# Every curve comes from a quality sweep; families with _420 and _444 settings take the better of the two at each
# quality. Ratios are geometric means over the levels Lift's steps 4 to 32 cover, wherever both curves reach.
#
# usage: compare_modern.py [--metric NAME] [--timing CSV] [--metrics CSV] [--jobs N] [--parts N] [images...]
#        compare_modern.py --lossless CSV
# --metrics without --timing runs or reuses a metrics pass, whose untimed sizes.csv stands in for the timing CSV, so
# only sizes are shown. The metrics pass scores with ssimulacra2 (and butteraugli with --metric butteraugli) using the
# CPUs this process may run on, so pin it with taskset. The timing pass must run alone: taskset -c 3.

import argparse, concurrent.futures, csv, glob, math, os, subprocess, sys, threading, time
from collections import defaultdict

HERE     = os.path.dirname(os.path.abspath(__file__))
ROOT     = os.path.dirname(HERE)
TOOL     = os.path.join(HERE, "build/compare_modern")
FAMILIES = [("stb", "stb_image_write and stb_image"), ("turbo", "libjpeg-turbo"), ("turbo_progressive", "libjpeg-turbo, progressive"),
            ("jpegli", "jpegli (libjpeg-turbo decode)"), ("webp", "WebP, method 4"), ("webp_m6", "WebP, method 6"),
            ("jxl", "JPEG XL, effort 7"), ("avif", "AVIF, aom speed 6, dav1d")]
ABLATION = [("jxl", "JPEG XL, effort 7"), ("jxl_noepf", "no EPF"), ("jxl_nogab", "no Gaborish"), ("jxl_nofilt", "no EPF, no Gaborish"),
            ("jxl_e3", "effort 3"), ("jxl_e5", "effort 5"), ("jxl_noxyb", "no XYB (VarDCT on RGB)"),
            ("jxl_noperc", "no XYB, no perceptual heuristics"), ("jxl_nopatch", "no patches, no dots"), ("jxl_dec4", "decoding speed 4")]
LOSSLESS = [("png", "PNG (source file, stb_image)"), ("webp_lossless", "WebP lossless, -z 6"), ("jxl_lossless", "JPEG XL lossless, effort 7")]
# name, column title, whether higher is better
METRICS  = [("psnr", "PSNR", True), ("ssimulacra2", "SSIMULACRA2", True), ("butteraugli", "Butteraugli", False)]
LEVEL_STEPS = (4, 8, 16, 32)

def score(reference, test, names):
	values = {"ssimulacra2": float("nan"), "butteraugli": float("nan")}
	if "ssimulacra2" in names:
		out = subprocess.run(["ssimulacra2", reference, test], capture_output=True, text=True).stdout.split()
		values["ssimulacra2"] = float(out[-1])
	if "butteraugli" in names:
		out = subprocess.run(["butteraugli_main", reference, test, "--pnorm", "3"], capture_output=True, text=True).stdout
		values["butteraugli"] = next(float(l.split()[-1]) for l in out.splitlines() if "norm" in l)
	os.remove(test)
	return values

# Splits the images over parts encoder processes, writes their untimed lines to sizes.csv and the scores to metrics.csv
def run_metrics(images, names, out_dir, jobs, parts, mode_flags):
	sources = {os.path.splitext(os.path.basename(p))[0]: p for p in images}
	dump    = os.path.join(out_dir, "dump")
	os.makedirs(dump, exist_ok=True)
	jobs_by_key, lines, lock = {}, [], threading.Lock()
	with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
		def feed(part):
			process = subprocess.Popen([TOOL] + mode_flags + ["-p", f"{part}/{parts}", "-d", dump] + images, stdout=subprocess.PIPE, text=True)
			for line in process.stdout:
				setting, param, image = line.split(",")[:3]
				with lock:
					lines.append(line)
					jobs_by_key[(setting, param, image)] = pool.submit(score, sources[image], os.path.join(dump, f"{setting}_{param}_{image}.png"), names)
			if process.wait(): raise SystemExit(f"compare_modern part {part} failed")
		feeders = [threading.Thread(target=feed, args=(p,)) for p in range(parts)]
		for t in feeders: t.start()
		for t in feeders: t.join()
		path = os.path.join(out_dir, "metrics.csv")
		with open(path, "w") as out:
			for (setting, param, image), job in sorted(jobs_by_key.items()):
				v = job.result()
				out.write(f"{setting},{param},{image},{v['ssimulacra2']},{v['butteraugli']},nan\n")
	with open(os.path.join(out_dir, "sizes.csv"), "w") as out: out.writelines(sorted(lines))
	print(f"metrics saved to {path}, untimed sizes to {os.path.join(out_dir, 'sizes.csv')}", file=sys.stderr)
	return path

# Per setting, points of pooled bpp and speeds, and each metric's mean over the images
def curves(timing_path, metrics_path):
	extra = {}
	if metrics_path:
		for setting, param, image, s2, ba, _ in csv.reader(open(metrics_path)):
			extra[(setting, float(param), image)] = {"ssimulacra2": float(s2), "butteraugli": float(ba)}
	rows = defaultdict(list)
	for setting, param, image, pixels, size, psnr, encode_s, decode_s in csv.reader(open(timing_path)):
		rows[(setting, float(param))].append((image, int(pixels), int(size), float(psnr), float(encode_s), float(decode_s)))
	out = defaultdict(list)
	for (setting, param), r in rows.items():
		pixels = sum(x[1] for x in r)
		point  = {"step": param, "bpp": sum(x[2] for x in r) * 8 / pixels, "psnr": sum(x[3] for x in r) / len(r),
		          "encode": pixels / sum(x[4] for x in r), "decode": pixels / sum(x[5] for x in r)}
		for name, _, _ in METRICS[1:]:
			values = [extra[(setting, param, x[0])][name] for x in r if (setting, param, x[0]) in extra]
			if len(values) == len(r) and not any(math.isnan(v) for v in values): point[name] = sum(values) / len(values)
		out[setting].append(point)
	return out, len({x[0] for r in rows.values() for x in r})

# Log-linear interpolation of a curve's values at a metric's level, or None outside it
def at(points, metric, level):
	points = sorted((p for p in points if metric in p), key=lambda p: p[metric])
	for a, b in zip(points, points[1:]):
		if a[metric] <= level <= b[metric] and a[metric] < b[metric]:
			t = (level - a[metric]) / (b[metric] - a[metric])
			return {k: math.exp(math.log(a[k]) + (math.log(b[k]) - math.log(a[k])) * t) for k in ("bpp", "encode", "decode")}
	return None

def family_at(all_curves, family, metric, level):
	options = [at(all_curves[s], metric, level) for s in (family, family + "_420", family + "_444") if s in all_curves]
	options = [o for o in options if o]
	return min(options, key=lambda o: o["bpp"]) if options else None

# Geometric mean of Lift against a family over the levels Lift's steps 4 to 32 span, for keys at that metric, and the
# share of those levels the family reaches
def ratios(all_curves, family, metric, keys, reference="lift"):
	lift   = [p for p in all_curves["lift"] if 4 <= p["step"] <= 32 and metric in p]
	if len(lift) < 2: return None
	low, high = min(p[metric] for p in lift), max(p[metric] for p in lift)
	logs, reached = defaultdict(list), 0
	for i in range(41):
		level = low + (high - low) * i / 40
		a, b  = family_at(all_curves, reference, metric, level), family_at(all_curves, family, metric, level)
		if a and b:
			reached += 1
			for k in keys: logs[k].append(math.log(a[k] / b[k]))
	if not logs: return None
	out = {k: math.exp(sum(v) / len(v)) for k, v in logs.items()}
	out["coverage"] = reached / 41
	return out

def size(ratio):
	return "n/a" if ratio is None else f"{1 - ratio:.0%} smaller" if ratio < 1 else f"{ratio - 1:.0%} larger"

def speed(ratio):
	return f"{ratio:.1f}× faster" if ratio >= 1 else f"{1 / ratio:.1f}× slower"

def lossy_tables(all_curves, count, metric, timed, families):
	shown  = [m for m in METRICS if m[0] == "psnr" or m[0] == metric]
	header = "| Lift Image versus | " + " | ".join(f"File size at the same {title}" for _, title, _ in shown) + " | Coverage |" + (" Decoding | Encoding |" if timed else "")
	print(header)
	print("|---" * (len(shown) + 2 + (2 if timed else 0)) + "|")
	for family, label in families:
		matched = ratios(all_curves, family, metric, ("bpp", "encode", "decode"))
		if not matched: continue
		sizes = [size((ratios(all_curves, family, name, ("bpp",)) or {}).get("bpp")) for name, _, _ in shown]
		row   = f"| {label} | " + " | ".join(sizes) + f" | {matched['coverage']:.0%} |"
		if timed: row += f" {speed(matched['decode'])} | {speed(matched['encode'])} |"
		print(row)
	title = next(t for n, t, _ in METRICS if n == metric)
	print(f"\n{count} images. Coverage is the share of Lift's {title} span (steps 4 to 32) the family reaches." + (f" Speeds at equal {title}." if timed else ""))

	# Lift's bpp over each family's at the level Lift reaches at a few steps
	lift   = {p["step"]: p for p in all_curves["lift"]}
	levels = [(s, lift[s][metric]) for s in LEVEL_STEPS if s in lift and metric in lift[s]]
	print(f"\n| Lift size / family size at {title} | " + " | ".join(f"step {s} ({v:.1f})" for s, v in levels) + " |")
	print("|---" * (len(levels) + 1) + "|")
	for family, label in families:
		cells = []
		for s, v in levels:
			a, b = at(all_curves["lift"], metric, v), family_at(all_curves, family, metric, v)
			cell = "n/a" if not (a and b) else f"{a['bpp'] / b['bpp']:.2f}"
			if timed and a and b: cell += f", dec {a['decode'] / b['decode']:.1f}×"
			cells.append(cell)
		print(f"| {label} | " + " | ".join(cells) + " |")
	if families is ABLATION:
		names = [m for m in METRICS[1:] if all(m[0] in p for c in all_curves.values() for p in c)]
		print("\n| JPEG XL variant | " + " | ".join(f"size vs default JPEG XL at equal {t}" for _, t, _ in names) + " | " + " | ".join(f"Lift size vs it at equal {t}" for _, t, _ in names) + " |")
		print("|---" * (2 * len(names) + 1) + "|")
		for family, label in families[1:]:
			versus_jxl  = [(ratios(all_curves, "jxl", n, ("bpp",), reference=family) or {}).get("bpp") for n, _, _ in names]
			versus_lift = [(ratios(all_curves, family, n, ("bpp",)) or {}).get("bpp") for n, _, _ in names]
			print(f"| {label} | " + " | ".join(size(r) for r in versus_jxl + versus_lift) + " |")
	print("\n| Lift step | bpp | " + title + " | PSNR |" + (" dec MP/s |" if timed else ""))
	print("|---|---|---|---|" + ("---|" if timed else ""))
	for s, _ in levels:
		p = lift[s]
		print(f"| {s:g} | {p['bpp']:.3f} | {p[metric]:.2f} | {p['psnr']:.2f} |" + (f" {p['decode'] / 1e6:.0f} |" if timed else ""))

def lossless_table(path, timed):
	rows = defaultdict(lambda: defaultdict(lambda: [0, 0, 0.0]))
	for setting, param, image, pixels, file_bytes, psnr, encode_s, decode_s in csv.reader(open(path)):
		for corpus in ("kodak" if image.startswith("kodim") else "examples", "all"):
			r = rows[corpus][setting]
			r[0] += int(pixels); r[1] += int(file_bytes); r[2] += float(decode_s)
	for corpus in ("examples", "kodak", "all"):
		lift = rows[corpus]["lift"]
		print(f"\n{corpus}\n\n| codec | bytes | bpp | Lift size vs |" + (" dec MP/s | Lift decode vs |" if timed else ""))
		print("|---|---|---|---|" + ("---|---|" if timed else ""))
		for setting, label in [("lift", "Lift step 0")] + LOSSLESS:
			r = rows[corpus][setting]
			if not r[0]: continue
			row = f"| {label} | {r[1]:,} | {r[1] * 8 / r[0]:.3f} | {size(lift[1] / r[1]) if setting != 'lift' else ''} |"
			if timed: row += f" {r[0] / r[2] / 1e6:.0f} | {speed(r[2] / lift[2]) if setting != 'lift' else ''} |"
			print(row)

def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--metric", default="ssimulacra2", choices=[m[0] for m in METRICS])
	parser.add_argument("--timing")
	parser.add_argument("--metrics")
	parser.add_argument("--lossless")
	parser.add_argument("--ablation", action="store_true", help="JPEG XL with single tools off, scored with Butteraugli too")
	parser.add_argument("--untimed", action="store_true", help="the lossless CSV came from an unpinned run, so show sizes only")
	parser.add_argument("--out", default=os.path.join(HERE, "runs", time.strftime("%Y%m%d_%H%M%S")))
	parser.add_argument("--jobs", type=int, default=9)
	parser.add_argument("--parts", type=int, default=3)
	parser.add_argument("images", nargs="*")
	args   = parser.parse_args()
	if args.lossless:
		lossless_table(args.lossless, not args.untimed)
		return
	images = args.images or sorted(glob.glob(os.path.join(ROOT, "corpus/examples/*.png")) + glob.glob(os.path.join(ROOT, "corpus/kodak/*.png")))
	scored  = ["ssimulacra2"] + (["butteraugli"] if args.metric == "butteraugli" or args.ablation else [])
	if not args.metrics: os.makedirs(args.out, exist_ok=True)
	metrics = args.metrics or run_metrics(images, scored, args.out, args.jobs, args.parts, ["-a"] if args.ablation else [])
	timing  = args.timing or os.path.join(os.path.dirname(metrics), "sizes.csv")
	all_curves, count = curves(timing, metrics)
	lossy_tables(all_curves, count, args.metric, args.timing is not None, ABLATION if args.ablation else FAMILIES)

if __name__ == "__main__": main()
