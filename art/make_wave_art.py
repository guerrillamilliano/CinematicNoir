#!/usr/bin/env python3
"""Draws the PLAY screen's waveform pieces (Pillow), all transparent PNGs in art/:
  wv_00..wv_11.png    one waveform column per height (w00..w39 pick one each): a symmetric amber bar
  head_00..head_40.png  the playhead over the whole waveform: 00 nothing, n a lit band up to column n with a bright line
  trig_0.png / trig_1.png  the TRIG lamp, dark and lit
Sizes must agree with layout.conf (the waveform box is COLS x COL_W by H). Run from the port folder:
    python3 art/make_wave_art.py"""
import os
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
COLS, COL_W, H, LEVELS = 40, 25, 84, 12
BAR_W = 15


def save(img, name):
    img.save(os.path.join(HERE, name))


# columns: a bar from the centre out, brighter in its middle, with soft ends
for lv in range(LEVELS):
    img = Image.new("RGBA", (COL_W, H), (0, 0, 0, 0))
    if lv:
        d = ImageDraw.Draw(img)
        half = round(lv * (H / 2 - 3) / (LEVELS - 1))
        x0 = (COL_W - BAR_W) // 2
        d.rounded_rectangle([x0, H // 2 - half, x0 + BAR_W - 1, H // 2 + half], radius=2, fill=(190, 128, 48, 235))
        d.rectangle([x0 + 4, H // 2 - half + 1, x0 + BAR_W - 5, H // 2 + half - 1], fill=(232, 166, 72, 255))
    save(img, "wv_%02d.png" % lv)

# playhead: the played part glows, the head is a bright line with a halo
W = COLS * COL_W
for n in range(COLS + 1):
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    if n:
        x = n * COL_W - COL_W // 2
        band = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(band).rectangle([0, 2, x, H - 3], fill=(255, 176, 70, 46))
        halo = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(halo).rectangle([x - 3, 0, x + 3, H - 1], fill=(255, 190, 90, 200))
        halo = halo.filter(ImageFilter.GaussianBlur(5))
        img = Image.alpha_composite(img, band)
        img = Image.alpha_composite(img, halo)
        ImageDraw.Draw(img).rectangle([x - 1, 0, x + 1, H - 1], fill=(255, 226, 160, 255))
    save(img, "head_%02d.png" % n)

# TRIG lamp
for on in (0, 1):
    S = 36
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    if on:
        glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        ImageDraw.Draw(glow).ellipse([4, 4, S - 5, S - 5], fill=(255, 160, 40, 220))
        img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(4)))
    d = ImageDraw.Draw(img)
    d.ellipse([9, 9, S - 10, S - 10], fill=(255, 214, 140, 255) if on else (58, 38, 14, 255), outline=(20, 14, 6, 255), width=2)
    if on:
        d.ellipse([13, 12, 18, 16], fill=(255, 245, 225, 255))
    save(img, "trig_%d.png" % on)
print("art: %d columns, %d playhead frames, 2 lamps" % (LEVELS, COLS + 1))
