#!/usr/bin/env python3
"""Draws what the engine shows live onto the PLAY tab's offline preview, so the screen can be judged as it will look:
sample text in MPC's own font at the layout's sizes and colours, a waveform (Dark Grand's shape) from the column images,
the playhead part-way through and the TRIG lamp lit. Run from the repo root after studio.py preview:
    python3 ports/noir/test/mock_text.py   -> ports/noir/build/mock_play.png"""
from PIL import Image, ImageDraw, ImageFont

ART = "ports/noir/art/"
FONTS = "tools/html_art/fonts/TitilliumWeb-%s.ttf"
Y = 86   # layout y -> skin y
img = Image.open("ports/noir/build/preview_0.png").convert("RGBA")

# waveform: Dark Grand's drawn picture, lit as while playing (wave_img 34), the playhead at column 14, the lamp lit
img.alpha_composite(Image.open(ART + "wimg_34.png").convert("RGBA"), (150, 262 - Y))
img.alpha_composite(Image.open(ART + "head_14.png").convert("RGBA"), (150, 262 - Y))
img.alpha_composite(Image.open(ART + "trig_1.png").convert("RGBA"), (1166, 282 - Y))

d = ImageDraw.Draw(img)
def text(cx, cy, w, s, size, colour, weight="SemiBold", align="center"):
    f = ImageFont.truetype(FONTS % weight, size)
    tw = d.textlength(s, font=f)
    x = cx - tw / 2 if align == "center" else cx - w / 2 + 8
    d.text((x, cy - Y), s, font=f, fill="#" + colour, anchor="lm")

# (cx, cy, w, text, size, colour, weight, align) from layout.conf
text(640, 118, 600, "CUTOFF  9.1 kHz", 19, "b8ad96", "Regular")
text(640, 226, 860, "Basement Grand *", 38, "ffc260", "Bold")
text(688, 370, 1076, "Dark Grand:  Felt-hammer concert grand, darkened highs, three strings per note", 21, "ffc260", align="left")
text(688, 402, 1076, "13 ZONES C0-C6   STEREO   ONE-SHOT   6.0 s   9.0 MB   DARK + GRITTY", 19, "e0a24a", align="left")
text(688, 442, 1076, "C3  VEL 104   ZONE C3   4 VOICES   GRITTY MODE", 21, "ffc260", align="left")
img.convert("RGB").save("ports/noir/build/mock_play.png")
print("wrote ports/noir/build/mock_play.png")
