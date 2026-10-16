#!/usr/bin/env python3
# Compares Lift Image with stb, libjpeg-turbo and mozjpeg at equal quality, and prints the README's table.
# Every curve comes from a quality sweep; each JPEG mode takes the better of 4:2:0 and 4:4:4 at each quality. Ratios
# are geometric means over the qualities Lift's steps 4 to 32 cover, wherever both curves reach. Speeds are compared
# at equal --metric, PSNR by default.
#
# usage: tools/compare.py [--perceptual] [--metric NAME] [--timing CSV] [--metrics CSV] [images...]
#   (corpus/examples and corpus/kodak by default)
# --perceptual adds SSIMULACRA2 and Butteraugli (ssimulacra2 and butteraugli_main, from libjxl's devtools) and FLIP
# (flip in PATH, or FLIP=<path>), scored in parallel on the other cores. --metric ssimulacra2, butteraugli or flip
# scores just that one. --timing and --metrics reuse the CSVs a run saves, whose paths it prints.
# Build first with make compare, adding MOZJPEG=<mozjpeg install prefix> for the mozjpeg rows. Timing follows the CPU
# it runs on, so pin it: taskset -c 3 tools/compare.py

import argparse, concurrent.futures, csv, glob, math, os, shutil, subprocess, sys, tempfile
from collections import defaultdict

ROOT     = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL     = os.path.join(ROOT, "build/jpeg_compare")
FAMILIES = [("stb", "stb_image_write and stb_image"), ("turbo", "libjpeg-turbo"), ("turbo_progressive", "libjpeg-turbo, progressive"),
            ("mozjpeg", "mozjpeg"), ("mozjpeg_psnr", "mozjpeg, tuned for PSNR")]
# name, column title, whether higher is better
METRICS  = [("psnr", "PSNR", True), ("ssimulacra2", "SSIMULACRA2", True), ("butteraugli", "Butteraugli", False), ("flip", "FLIP", False)]

def run_timing(images):
	path = os.path.join(tempfile.mkdtemp(), "timing.csv")
	with open(path, "w") as out:
		subprocess.run([TOOL] + images, stdout=out, check=True)
	print(f"timing saved to {path}", file=sys.stderr)
	return path

# Scorers may use every core, even when the timing run is pinned with taskset
def unpin():
	if hasattr(os, "sched_setaffinity"): os.sched_setaffinity(0, range(os.cpu_count() or 1))

def score(reference, test, names):
	values = {name: float("nan") for name, _, _ in METRICS[1:]}
	if "ssimulacra2" in names:
		out = subprocess.run(["ssimulacra2", reference, test], capture_output=True, text=True, preexec_fn=unpin).stdout.split()
		values["ssimulacra2"] = float(out[-1])
	if "butteraugli" in names:
		out = subprocess.run(["butteraugli_main", reference, test, "--pnorm", "3"], capture_output=True, text=True, preexec_fn=unpin).stdout
		values["butteraugli"] = next(float(l.split()[-1]) for l in out.splitlines() if "norm" in l)
	if "flip" in names:
		flip = os.environ.get("FLIP") or shutil.which("flip")
		out = subprocess.run([flip, "-r", reference, "-t", test, "-nexm", "-nerm"], capture_output=True, text=True, env={**os.environ, "OMP_NUM_THREADS": "1"}, preexec_fn=unpin).stdout
		values["flip"] = next(float(l.split()[-1]) for l in out.splitlines() if l.strip().startswith("Mean:"))
	os.remove(test)
	return values

def run_metrics(images, names):
	sources = {os.path.splitext(os.path.basename(p))[0]: p for p in images}
	dump    = tempfile.mkdtemp()
	path    = os.path.join(dump, "metrics.csv")
	jobs    = {}
	with concurrent.futures.ThreadPoolExecutor(max(1, (os.cpu_count() or 2) - 1)) as pool:
		process = subprocess.Popen([TOOL, "-d", dump] + images, stdout=subprocess.PIPE, text=True, preexec_fn=unpin)
		for line in process.stdout:
			setting, param, image = line.split(",")[:3]
			jobs[(setting, param, image)] = pool.submit(score, sources[image], os.path.join(dump, f"{setting}_{param}_{image}.png"), names)
		process.wait()
		with open(path, "w") as out:
			for (setting, param, image), job in jobs.items():
				v = job.result()
				out.write(f"{setting},{param},{image},{v['ssimulacra2']},{v['butteraugli']},{v['flip']}\n")
	print(f"metrics saved to {path}", file=sys.stderr)
	return path

# Per setting, points of pooled bpp and speeds, and each metric's mean over the images
def curves(timing_path, metrics_path):
	extra = {}
	if metrics_path:
		for setting, param, image, s2, ba, fl in csv.reader(open(metrics_path)):
			extra[(setting, float(param), image)] = {"ssimulacra2": float(s2), "butteraugli": float(ba), "flip": float(fl)}
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

# Geometric mean of Lift against a family over the levels Lift's steps 4 to 32 span, for keys at that metric
def ratios(all_curves, family, metric, keys):
	lift   = [p for p in all_curves["lift"] if 4 <= p["step"] <= 32 and metric in p]
	if len(lift) < 2: return None
	low, high = min(p[metric] for p in lift), max(p[metric] for p in lift)
	logs = defaultdict(list)
	for i in range(41):
		level = low + (high - low) * i / 40
		a, b  = at(all_curves["lift"], metric, level), family_at(all_curves, family, metric, level)
		if a and b:
			for k in keys: logs[k].append(math.log(a[k] / b[k]))
	return {k: math.exp(sum(v) / len(v)) for k, v in logs.items()} if logs else None

def size(ratio):
	return "n/a" if ratio is None else f"{1 - ratio:.0%} smaller" if ratio < 1 else f"{ratio - 1:.0%} larger"

def speed(ratio):
	return f"{ratio:.1f}× faster" if ratio >= 1 else f"{1 / ratio:.1f}× slower"

def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--perceptual", action="store_true")
	parser.add_argument("--metric", default="psnr", choices=[m[0] for m in METRICS])
	parser.add_argument("--timing")
	parser.add_argument("--metrics")
	parser.add_argument("images", nargs="*")
	args   = parser.parse_args()
	images = args.images or sorted(glob.glob(os.path.join(ROOT, "corpus/examples/*.png")) + glob.glob(os.path.join(ROOT, "corpus/kodak/*.png")))
	timing  = args.timing or run_timing(images)
	scored  = [m[0] for m in METRICS[1:]] if args.perceptual else [args.metric] if args.metric != "psnr" else []
	metrics = args.metrics or (run_metrics(images, scored) if scored else None)
	all_curves, count = curves(timing, metrics)

	shown  = [m for m in METRICS if m[0] == "psnr" or m[0] == args.metric or (metrics and args.perceptual)]
	header = "| Lift Image versus | " + " | ".join(f"File size at the same {title}" for _, title, _ in shown) + " | Decoding | Encoding |"
	print(header)
	print("|---" * (len(shown) + 3) + "|")
	for family, label in FAMILIES:
		matched = ratios(all_curves, family, args.metric, ("bpp", "encode", "decode"))
		if not matched: continue
		sizes = [size((ratios(all_curves, family, name, ("bpp",)) or {}).get("bpp")) for name, _, _ in shown]
		print(f"| {label} | " + " | ".join(sizes) + f" | {speed(matched['decode'])} | {speed(matched['encode'])} |")
	print(f"\n{count} images, speeds at equal {next(title for name, title, _ in METRICS if name == args.metric)}.")

if __name__ == "__main__": main()
