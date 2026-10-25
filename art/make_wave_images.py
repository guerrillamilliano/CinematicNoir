#!/usr/bin/env python3
"""Draws a realistic waveform picture of each built-in sound for the PLAY screen (wave_img picks one):
  wimg_00.png          blank (a User sound: the engine shows its column waveform, w00..w39, instead)
  wimg_01..wimg_NN.png the rom sounds in folder order (the engine's order), dim: shown while nothing plays
  wimg_NN+1..2NN.png   the same, lit: shown while a note plays
Each is the zone that plays middle C, as the engine shows it (zones split halfway between roots): the min/max of the
samples at every pixel (both channels), a brighter RMS core, a thin bright edge and a soft glow, like a DAW's view.
Needs build/rom (tools/render_rom.c). Run from the port folder: python3 art/make_wave_images.py"""
import glob
import math
import os
import re
import struct
import wave
from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROM = os.path.join(os.path.dirname(HERE), "build", "rom")
W, H, SS = 1000, 84, 2          # size in the layout, and the supersampling for smooth edges
GAMMA = 0.85                    # a touch of lift so quiet tails still read


def zone_for_c3(folder):
    """The WAV that plays note 60: roots from the names' trailing numbers, each zone up to halfway to the next root."""
    files = sorted(glob.glob(os.path.join(folder, "*.wav")))
    roots = sorted((int(re.search(r"(\d+)\.wav$", f, re.I).group(1)), f) for f in files)
    for i, (r, f) in enumerate(roots):
        lo = 0 if i == 0 else (roots[i - 1][0] + r) // 2 + 1
        hi = 127 if i == len(roots) - 1 else (r + roots[i + 1][0]) // 2
        if lo <= 60 <= hi:
            return f
    return roots[0][1]


def read(path):
    w = wave.open(path)
    n, ch = w.getnframes(), w.getnchannels()
    s = struct.unpack("<%dh" % (n * ch), w.readframes(n))
    return [s[c::ch] for c in range(ch)], n


def draw(path):
    chans, n = read(path)
    cols = W * SS
    lo, hi, rms = [0.0] * cols, [0.0] * cols, [0.0] * cols
    for x in range(cols):
        a, b = n * x // cols, max(n * (x + 1) // cols, n * x // cols + 1)
        mn = mx = 0
        acc = 0.0
        cnt = 0
        for c in chans:
            seg = c[a:b]
            mn, mx = min(mn, min(seg)), max(mx, max(seg))
            acc += sum(v * v for v in seg[::4])
            cnt += len(seg[::4])
        lo[x], hi[x], rms[x] = mn / 32768.0, mx / 32768.0, math.sqrt(acc / max(cnt, 1)) / 32768.0
    top = max(max(hi), -min(lo), 1e-6)
    half = (H * SS) / 2 - 3 * SS
    mid = (H * SS) / 2
    def y(v):
        v = v / top
        return mid - math.copysign(abs(v) ** GAMMA, v) * half

    body = Image.new("RGBA", (cols, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(body)
    upper = [(x, y(hi[x])) for x in range(cols)]
    lower = [(x, y(lo[x])) for x in range(cols - 1, -1, -1)]
    d.polygon(upper + lower, fill=(196, 128, 44, 225))
    core_u = [(x, y(rms[x])) for x in range(cols)]
    core_l = [(x, y(-rms[x])) for x in range(cols - 1, -1, -1)]
    d.polygon(core_u + core_l, fill=(255, 186, 86, 255))
    d.line(upper, fill=(255, 222, 160, 255), width=SS)
    d.line([(x, y(lo[x])) for x in range(cols)], fill=(255, 222, 160, 200), width=SS)

    glow = body.filter(ImageFilter.GaussianBlur(4 * SS))
    glow.putalpha(glow.getchannel("A").point(lambda a: a * 0.55))
    out = Image.alpha_composite(glow, body)
    return out.resize((W, H), Image.LANCZOS)


def main():
    folders = sorted(d for d in glob.glob(os.path.join(ROM, "*")) if os.path.isdir(d))   # folder order = the engine's
    if not folders:
        raise SystemExit("make_wave_images: no build/rom (run tools/make_rom.ps1 first)")
    Image.new("RGBA", (W, H), (0, 0, 0, 0)).save(os.path.join(HERE, "wimg_00.png"))
    n = len(folders)
    for i, f in enumerate(folders, 1):
        lit = draw(zone_for_c3(f))
        lit.save(os.path.join(HERE, "wimg_%02d.png" % (i + n)))
        r, g, b, a = lit.split()   # dim: a faint, cooler ghost of the same waveform
        dim = Image.merge("RGBA", (r.point(lambda v: v * 0.62), g.point(lambda v: v * 0.55), b.point(lambda v: v * 0.5),
                                   a.point(lambda v: v * 0.42)))
        dim.save(os.path.join(HERE, "wimg_%02d.png" % i))
    print("art: %d waveform pictures, dim and lit" % n)


main()
