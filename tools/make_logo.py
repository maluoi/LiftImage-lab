#!/usr/bin/env python3
# Draws the Lift Image icon and README header into the codec's docs/images: a feather. Text becomes outlines, so the
# header looks the same without Inter.
# usage: tools/make_logo.py      (needs fontTools and the Inter fonts)
import math, os
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

ROOT    = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT     = os.path.join(ROOT, os.environ.get("CORE", "LiftImage"), "docs", "images")
FONTS   = "/usr/share/fonts/rsms-inter-fonts"

BACKGROUND = ("#17191e", "#262a31")             # top left to bottom right
FEATHER    = ("#9ea7b4", "#d3d9e1", "#f8f9fb")  # base to tip, brightening as it lifts
QUILL      = "#7d8796"
TEXT       = "#f4f1ec"

# The feather in local units, base at the origin and tip up at -240, before BEND curves it. Each vane is two cubics
# from the quill out and up to the tip; the right one is narrower, as on a flight feather
RIGHT_VANE = [(1, -16), (20, -40), (34, -112), (30, -162), (27, -202), (12, -229), (0, -240)]
LEFT_VANE  = [(-1, -30), (-27, -54), (-43, -122), (-38, -172), (-34, -210), (-14, -233), (0, -240)]
QUILL_PATH = [(0, 36), (0, -60), (0, -170), (0, -238)]
SPLITS     = [(RIGHT_VANE, -84, 0.62), (RIGHT_VANE, -138, 0.55), (LEFT_VANE, -112, 0.58)] # vane, height, depth toward the quill
BEND       = 22  # how far the tip curves right

def bezier(p, t):
	u = 1 - t
	return tuple(u**3 * p[0][i] + 3 * u * u * t * p[1][i] + 3 * u * t * t * p[2][i] + t**3 * p[3][i] for i in range(2))

def bend(point):
	x, y = point
	return (x + BEND * (y / 240) ** 2, y)

class Placement:
	def __init__(self, x, y, scale, angle_degrees):
		self.x, self.y, self.scale = x, y, scale
		self.cos, self.sin = math.cos(math.radians(angle_degrees)), math.sin(math.radians(angle_degrees))
	def __call__(self, point):
		px, py = bend(point)
		return (self.x + self.scale * (px * self.cos - py * self.sin), self.y + self.scale * (px * self.sin + py * self.cos))

def fmt(point): return f"{point[0]:.1f},{point[1]:.1f}"

def vane_path(vane, place):
	p = [place(v) for v in vane]
	return f"M{fmt(p[0])} C{fmt(p[1])} {fmt(p[2])} {fmt(p[3])} C{fmt(p[4])} {fmt(p[5])} {fmt(p[6])} Z"

# A vane's outer edge as sampled points, from the quill out to the tip
def outline(vane, count=200):
	return [bezier(curve, i / count) for curve in (vane[0:4], vane[3:7]) for i in range(count + 1)]

# A wedge cut from the edge in toward the quill and back toward the base, along the barbs
def split(vane, height, depth, place):
	ex, ey = min(outline(vane), key=lambda p: abs(p[1] - height))
	inner  = (ex * (1 - depth), ey + 30 * depth)
	return f"M{fmt(place((ex * 1.25, ey - 7)))} L{fmt(place((ex * 1.25, ey + 2)))} L{fmt(place(inner))} Z"

# Placed at unit scale first, then fitted and centered by its outline
def fit(x, y, size):
	trial  = Placement(0, 0, 1, 40)
	points = [trial(p) for p in outline(RIGHT_VANE) + outline(LEFT_VANE) + [QUILL_PATH[0], QUILL_PATH[-1]]]
	left, right  = min(p[0] for p in points), max(p[0] for p in points)
	top,  bottom = min(p[1] for p in points), max(p[1] for p in points)
	scale = size / max(right - left, bottom - top)
	return Placement(x + size / 2 - scale * (left + right) / 2, y + size / 2 - scale * (top + bottom) / 2, scale, 40)

# A feather fitted to a box of size units with its top left at x, y; ids keep two marks in one file apart
def mark(x, y, size, id):
	place = fit(x, y, size)
	start, end = place((0, 36)), place((0, -240))
	splits = " ".join(split(vane, height, depth, place) for vane, height, depth in SPLITS)
	q = [place(v) for v in QUILL_PATH]
	return f'''<linearGradient id="{id}feather" gradientUnits="userSpaceOnUse" x1="{start[0]:.1f}" y1="{start[1]:.1f}" x2="{end[0]:.1f}" y2="{end[1]:.1f}">
		<stop offset="0" stop-color="{FEATHER[0]}"/><stop offset="0.55" stop-color="{FEATHER[1]}"/><stop offset="1" stop-color="{FEATHER[2]}"/>
	</linearGradient>
	<mask id="{id}vanes" maskUnits="userSpaceOnUse" x="{x - size:.1f}" y="{y - size:.1f}" width="{3 * size:.1f}" height="{3 * size:.1f}">
		<rect x="{x - size:.1f}" y="{y - size:.1f}" width="{3 * size:.1f}" height="{3 * size:.1f}" fill="white"/>
		<path d="{splits}" fill="black"/>
	</mask>
	<g mask="url(#{id}vanes)" fill="url(#{id}feather)">
		<path d="{vane_path(RIGHT_VANE, place)}"/>
		<path d="{vane_path(LEFT_VANE, place)}"/>
	</g>
	<path d="M{fmt(q[0])} C{fmt(q[1])} {fmt(q[2])} {fmt(q[3])}" fill="none" stroke="{QUILL}" stroke-opacity="0.9" stroke-width="{4 * place.scale:.1f}" stroke-linecap="round"/>'''

def background(name):
	return (f'<linearGradient id="{name}" x1="0" y1="0" x2="1" y2="1">'
	        f'<stop offset="0" stop-color="{BACKGROUND[0]}"/><stop offset="1" stop-color="{BACKGROUND[1]}"/></linearGradient>')

# Text as one path with its baseline at y, and its advance width
def text(font_file, string, size, x, y, tracking=0.0):
	font   = TTFont(os.path.join(FONTS, font_file))
	glyphs = font.getGlyphSet()
	cmap   = font.getBestCmap()
	scale  = size / font["head"].unitsPerEm
	pen    = SVGPathPen(glyphs)
	cursor = 0.0
	for char in string:
		name = cmap[ord(char)]
		glyphs[name].draw(TransformPen(pen, (scale, 0, 0, -scale, x + cursor, y)))
		cursor += font["hmtx"][name][0] * scale + tracking * size
	return pen.getCommands(), cursor - tracking * size

def icon():
	return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">
	<title>Lift Image</title>
	<defs>{background("background")}</defs>
	<rect width="256" height="256" rx="56" fill="url(#background)"/>
	{mark(36, 36, 184, "icon")}
</svg>
'''

def header():
	w, h      = 1280, 320
	mark_size = 236
	_, name_w = text("InterDisplay-Bold.ttf", "Lift Image", 128, 0, 0, -0.01)
	left      = (w - mark_size - 36 - name_w) / 2
	name, _   = text("InterDisplay-Bold.ttf", "Lift Image", 128, left + mark_size + 36, 206, -0.01)
	return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}">
	<title>Lift Image</title>
	<defs>{background("background")}</defs>
	<rect width="{w}" height="{h}" rx="28" fill="url(#background)"/>
	{mark(left, (h - mark_size) / 2, mark_size, "header")}
	<path d="{name}" fill="{TEXT}"/>
</svg>
'''

os.makedirs(OUT, exist_ok=True)
open(os.path.join(OUT, "lift_icon.svg"),   "w").write(icon())
open(os.path.join(OUT, "lift_header.svg"), "w").write(header())
print("wrote", OUT)
