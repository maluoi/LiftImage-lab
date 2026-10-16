#!/usr/bin/env python3
# Decodes example images at a few settings into examples/, and builds zoomed comparison sheets in examples/compare.
# usage: tools/examples.py

import csv, glob, os, shutil, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT  = os.path.join(ROOT, "examples")
FONT = subprocess.run(["fc-match", "-f", "%{file}", "Liberation Sans"], capture_output=True, text=True).stdout.strip()

GROUPS = {
	"ldr":   ["corpus/examples"],
	"modes": ["corpus/modes/rock_rough_gray.png", "corpus/modes/texture_normal_nrm.png", "corpus/modes/fern_rgba.png", "corpus/modes/fern_blackbg_rgba.png", "corpus/modes/metal_plate_arm_data.png"],
	"hdr":   ["corpus/hdr/brown_photostudio_02.hdr", "corpus/hdr/kloofendal_48d_partly_cloudy_puresky.hdr"],
}

# image: crop x, y, size, zoom, extra magick ops on the crop
CROPS = {
	"parrot":                               (380, 100, 192, 3, []),
	"screenshot_ui":                        (930, 120, 192, 3, []),
	"screenshot_3d":                        (560,  90, 192, 3, []),
	"texture_normal":                       (200, 200, 192, 3, []),
	"texture_aerial_rocks_02":              (200, 200, 192, 3, []),
	"texture_bark_willow":                  (200, 200, 192, 3, []),
	"texture_brick_wall_001":               (200, 200, 192, 3, []),
	"texture_metal_plate":                  (200, 200, 192, 3, []),
	"texture_rock_face_03":                 (200, 200, 192, 3, []),
	"texture_wood_floor_deck":              (200, 200, 192, 3, []),
	"rock_rough_gray":                      (200, 200, 192, 3, []),
	"texture_normal_nrm":                   (200, 200, 192, 3, []),
	"metal_plate_arm_data":                 (160, 160, 192, 3, []),
	"fern_rgba":                            (150,  60, 192, 3, ["checker"]),
	"fern_blackbg_rgba":                    (150,  60, 192, 3, ["checker"]),
	"brown_photostudio_02":                 (450, 190, 256, 2, []),
	"kloofendal_48d_partly_cloudy_puresky": (650, 150, 256, 2, []),
}

def run(group):
	d = os.path.join(OUT, group)
	shutil.rmtree(d, ignore_errors=True)
	os.makedirs(d)
	subprocess.run([os.path.join(ROOT, "build/bench"), "-d", d, "-c", os.path.join(d, "stats.csv")] + [os.path.join(ROOT, p) for p in GROUPS[group]], cwd=ROOT, check=True, capture_output=True)
	for f in glob.glob(os.path.join(d, "*_lfi0*.png")) + glob.glob(os.path.join(d, "*_lfi8_ev*.png")):
		os.remove(f)
	return {(r["image"], r["codec"], float(r["param"])): r for r in csv.DictReader(open(os.path.join(d, "stats.csv")))}

# SSIMULACRA2 of a whole decoded image, when libjxl's ssimulacra2 tool is installed
def ssimulacra2(reference, test):
	if not shutil.which("ssimulacra2"): return None
	return float(subprocess.run(["ssimulacra2", reference, test], capture_output=True, text=True, check=True).stdout.split()[-1])

def label(stats, image, codec, param, kind, group=None):
	r = stats.get((image, codec, float(param)))
	if codec == "src" or not r: return "source"
	bpp = int(r["bytes"]) * 8 / (int(r["w"]) * int(r["h"]))
	name = f"{codec} q{param:g}" if codec == "jpg" else f"lfi step {param:g}"
	if kind == "normal": return f"{name}   {bpp:.3f} bpp  {float(r['angle']):.2f} deg"
	if kind == "alpha":  return f"{name}   {bpp:.3f} bpp  {float(r['psnr']):.2f} dB  alpha {float(r['alpha']):.1f}"
	if kind == "hdr":    return f"{name}   {bpp:.3f} bpp  {float(r['psnr']):.2f} dB  {float(r['stops']):.3f} stops"
	score = ssimulacra2(os.path.join(OUT, group, f"{image}_src0.png"), os.path.join(OUT, group, f"{image}_{codec}{param:g}.png")) if group else None
	return f"{name}   {bpp:.3f} bpp  {float(r['psnr']):.2f} dB" + (f"  SSIMULACRA2 {score:.0f}" if score is not None else "")

def tile(path, crop, text, out):
	x, y, size, zoom, extra = crop
	cmd = ["magick", path, "-crop", f"{size}x{size}+{x}+{y}", "+repage"]
	if "checker" in extra:
		cmd = ["magick", "-size", f"{size}x{size}", "pattern:checkerboard", "(", path, "-crop", f"{size}x{size}+{x}+{y}", "+repage", ")", "-compose", "over", "-composite"]
	cmd += [e for e in extra if e != "checker"]
	cmd += ["-filter", "point", "-resize", f"{zoom * 100}%", "-gravity", "north", "-background", "#202020", "-fill", "white",
		"-font", FONT, "-pointsize", "18", "-splice", "0x28", "-annotate", "+0+4", text, out]
	subprocess.run(cmd, check=True)

def sheet(rows, out):
	parts = []
	for row in rows: parts += ["("] + row + ["+append", ")"]
	subprocess.run(["magick"] + parts + ["-append", "+repage", out], check=True)
	for row in rows:
		for t in row: os.remove(t)

def main():
	cmp = os.path.join(OUT, "compare")
	shutil.rmtree(OUT, ignore_errors=True)
	os.makedirs(cmp)
	stats = {g: run(g) for g in GROUPS}

	ldr = [("src", 0), ("jpg", 75), ("jpg", 90), ("lfi", 8), ("lfi", 16), ("lfi", 32)]
	for image in sorted({k[0] for k in stats["ldr"]}):
		tiles = [os.path.join(cmp, f"_{i}.png") for i in range(len(ldr))]
		for (codec, param), t in zip(ldr, tiles):
			tile(os.path.join(OUT, "ldr", f"{image}_{codec}{param:g}.png"), CROPS[image], label(stats["ldr"], image, codec, param, "ldr", "ldr"), t)
		sheet([tiles[:3], tiles[3:]], os.path.join(cmp, f"{image}.png"))

	modes = [("src", 0), ("lfi", 8), ("lfi", 16), ("lfi", 32)]
	for image in sorted({k[0] for k in stats["modes"]}):
		kind  = "normal" if "nrm" in image else "alpha" if "rgba" in image else "ldr"
		tiles = [os.path.join(cmp, f"_{i}.png") for i in range(len(modes))]
		for (codec, param), t in zip(modes, tiles):
			tile(os.path.join(OUT, "modes", f"{image}_{codec}{param:g}.png"), CROPS[image], label(stats["modes"], image, codec, param, kind), t)
		sheet([tiles[:2], tiles[2:]], os.path.join(cmp, f"{image}.png"))

	hdr = [("src", 0), ("lfi", 16), ("lfi", 32), ("lfi", 64)]
	for image in sorted({k[0] for k in stats["hdr"]}):
		rows = []
		for ev in ("-2", "+0", "+2"):
			row = []
			for i, (codec, param) in enumerate(hdr):
				t = os.path.join(cmp, f"_{ev}_{i}.png")
				name = "src" if codec == "src" else f"{codec}{param:g}"
				tile(os.path.join(OUT, "hdr", f"{image}_{name}_ev{ev}.png"), CROPS[image], f"ev {ev}   " + label(stats["hdr"], image, codec, param, "hdr"), t)
				row.append(t)
			rows.append(row)
		sheet(rows, os.path.join(cmp, f"{image}.png"))

if __name__ == "__main__": main()
