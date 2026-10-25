#!/usr/bin/env python3
"""Composites the BUILT skin's picture images (TUI.json Image components switched by IndexedEnabling, in component
order, as MPC stacks them) onto the PLAY preview for a given state, plus sample live text: checks the waveform, playhead
and lamp layer the way the device draws them, with the files the device gets. Run from the repo root after studio.py:
    python3 ports/noir/test/composite_screen.py [wave_img=34 wave_head=14 trig=1]   -> ports/noir/build/screen_check.png"""
import json
import re
import sys
from PIL import Image, ImageDraw, ImageFont

SK = "ports/noir/build/skin/Milliano - VST - Cinematic Noir/Plugin Skins/"
keys = [p["key"] for p in json.load(open("ports/noir/params.json"))["params"]]
state = {"wave_img": 34, "wave_head": 14, "trig": 1}
for a in sys.argv[1:]:
    k, v = a.split("=")
    state[k] = int(v)
want = {keys.index(k): v for k, v in state.items()}
for i in range(40):   # the column bars sit at 0 unless set
    want.setdefault(keys.index("w%02d" % i), 0)

tui = json.load(open(SK + "TUI.json"))
local = tui["pageData"]["componentDefinitions"]["localComponentDefinitions"]
img = Image.open("ports/noir/build/preview_0.png").convert("RGBA")
n = 0
for d in local:
    for c in (d.get("value") or {}).get("componentsData", []) if isinstance(d.get("value"), dict) else []:
        hs = c["bounds"].get("additionalInvalidatingHandles") or []
        m = re.match(r"IndexedEnabling/(\d+)/\d+/Parameter (\d+)$", hs[0]) if len(hs) == 1 else None
        if c["componentData"]["type"] != "Image" or not m or want.get(int(m.group(2))) != int(m.group(1)):
            continue
        x, y, w, h = (int(v) for v in c["bounds"]["bounds"].split())
        im = Image.open(SK + c["componentData"]["data"]["image"]).convert("RGBA").resize((w, h))
        img.alpha_composite(im, (x, y))
        n += 1

d = ImageDraw.Draw(img)
F = "tools/html_art/fonts/TitilliumWeb-%s.ttf"
def text(cx, cy, w, s, size, colour, weight="SemiBold", align="center"):
    f = ImageFont.truetype(F % weight, size)
    x = cx - d.textlength(s, font=f) / 2 if align == "center" else cx - w / 2 + 8
    d.text((x, cy - 86), s, font=f, fill="#" + colour, anchor="lm")
text(650, 118, 500, "CUTOFF  9.1 kHz", 22, "b8ad96", "Regular")
text(640, 226, 860, "Basement Grand *", 44, "ffc260", "Bold")
text(688, 370, 1076, "Dark Grand:  Felt-hammer concert grand, darkened highs, three strings per note", 24, "ffc260", align="left")
text(688, 402, 1076, "13 ZONES C0-C6   STEREO   ONE-SHOT   6.0 s   9.0 MB   DARK + GRITTY", 22, "e0a24a", align="left")
text(688, 442, 1076, "C3  VEL 104   ZONE C3   4 VOICES   GRITTY MODE", 24, "ffc260", align="left")
img.convert("RGB").save("ports/noir/build/screen_check.png")
print("screen_check.png: %d picture images drawn for %s" % (n, state))
