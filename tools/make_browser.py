#!/usr/bin/env python3
"""MPC's own preset browser for Cinematic Noir (docs/NOTES.md "Instruments-browser tiles"):
  art/tile.png                                  the 270x110 Sounds > INSTRUMENTS tile (vst.json "tile")
  build/xpl/<Category>/<Instrument>/<Preset>.xpl one MPC preset per engine preset, in folders by category and instrument
Each .xpl carries the wrapper's chunk: the preset's state string (src/presets.h) plus its preset and category, so loading
it from the browser sets the whole sound. The release ships each category folder as Presets/<Category>.
Each .xpl names the plugin as plugin-meta.xml does, with the %payload-path% placeholder the installer fills in (for these
subfolders after tools/patch_release.py), or by a real path with --synths. Uses the repo's tools/xpl.py. Run from the port
folder with the repo mounted: python3 tools/make_browser.py [--synths <folder>]"""
import argparse
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import xpl   # noqa: E402

CATS = ["Keys", "Strings", "Choir", "Brass", "Pads", "Synth", "Texture", "Leads"]   # params.json "category" order

ap = argparse.ArgumentParser()
ap.add_argument("--synths", default="%payload-path%", help="the Synths folder (default: the installer's placeholder)")
a = ap.parse_args()

cfg = json.load(open(os.path.join(HERE, "vst.json")))
skin = "%s - VST - %s" % (cfg["vendor"], cfg["name"])
so_path = "%s/%s/%s" % (a.synths.rstrip("/"), skin, cfg["so"])
uid = int.from_bytes(cfg["uid"].encode(), "big")

src = open(os.path.join(HERE, "src", "presets.h")).read()
presets = re.findall(r'\{ "((?:[^"\\]|\\.)*)", (\d+), \d+, "(?:[^"\\]|\\.)*", "((?:[^"\\]|\\.)*)" \}', src)   # name, cat, mode, sound, state
if not presets:
    raise SystemExit("make_browser: no presets in src/presets.h (run tools/gen_presets.py)")

out = os.path.join(HERE, "build", "xpl")
shutil.rmtree(out, ignore_errors=True)
safe = lambda s: re.sub(r'[\\/:*?"<>|]', "-", s).strip()
for n, (name, cat, state) in enumerate(presets):
    name, state, cat = name.replace('\\"', '"'), state.replace('\\"', '"'), int(cat)
    sound = re.search(r"sname=([^;]*);", state).group(1)
    inst = sound.split(" - ", 1)[1] if " - " in sound else sound
    chunk = ("%spreset=%d;category=%d;dirty=0;" % (state, n, cat)).encode() + b"\0"
    d = os.path.join(out, CATS[cat], safe(inst))
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, safe(name) + ".xpl"), "w", encoding="utf-8") as f:
        f.write(xpl.xpl(cfg["name"], cfg["vendor"], so_path, uid, cfg.get("version", 1000), chunk, preset=name))
print("build/xpl: %d presets in %d categories (plugin at %s)" % (len(presets), len(set(int(c) for _, c, _ in presets)), so_path))

# the tile: the title over Dark Grand's waveform, in the screen's amber on black
try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("make_browser: Pillow is needed for the tile")
fonts = os.path.join(HERE, "..", "..", "tools", "html_art", "fonts", "TitilliumWeb-%s.ttf")
tile = Image.new("RGBA", (270, 110), (10, 9, 8, 255))
d = ImageDraw.Draw(tile)
for y in range(110):   # a faint amber glow behind
    d.line([(0, y), (270, y)], fill=(30 + int(14 * (1 - abs(y - 55) / 55)), 20 + int(8 * (1 - abs(y - 55) / 55)), 8, 255))
import glob
n_pics = len(glob.glob(os.path.join(HERE, "art", "wimg_*.png")))   # blank + dim + lit
wave = os.path.join(HERE, "art", "wimg_%02d.png" % ((n_pics - 1) // 2 + 1))   # Dark Grand (the first rom sound), lit
if os.path.exists(wave):
    w = Image.open(wave).convert("RGBA").resize((250, 42))
    w.putalpha(w.getchannel("A").point(lambda v: v * 0.6))
    tile.alpha_composite(w, (10, 58))
d.text((14, 10), "CINEMATIC", font=ImageFont.truetype(fonts % "Bold", 30), fill=(242, 230, 200, 255))
d.line([(15, 50), (52, 50)], fill=(201, 164, 92, 255), width=2)
d.text((60, 40), "N O I R", font=ImageFont.truetype(fonts % "Regular", 18), fill=(201, 164, 92, 255))
d.rectangle([0, 0, 269, 109], outline=(60, 50, 34, 255))
tile.convert("RGB").save(os.path.join(HERE, "art", "tile.png"))
print("art/tile.png")
