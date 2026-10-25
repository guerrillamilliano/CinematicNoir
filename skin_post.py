#!/usr/bin/env python3
"""vst.json "skin_post": run by gen_vst.py as `python3 skin_post.py <plugin folder>` on the built skin; fails when a target
is missing.

The PLAY screen's waveform is pictures stacked in one box: the column bars (w00..w39), the drawn waveform (wave_img), the
playhead (wave_head) on top, and the TRIG lamp. The builder bakes each picture frame opaque (the background redrawn under
it), so the playhead's frames, full-width, blanked the waveform under them. Each frame's image is replaced here by the
transparent PNG it came from (art/, MPC honours PNG alpha, as SurgiQ's pills), at exactly its box, so the layers show
through each other."""
import json
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, "art")
keys = [p["key"] for p in json.load(open(os.path.join(HERE, "params.json")))["params"]]
Y = 86   # layout y -> skin y

# parameter -> (file pattern for an option, its box in layout coordinates); boxes agree with layout.conf
PICS = {"wave_img": ("wimg_%02d.png", (150, 262, 1000, 84)),
        "wave_head": ("head_%02d.png", (150, 262, 1000, 84)),
        "trig": ("trig_%d.png", (1166, 282, 36, 36))}
for i in range(40):
    PICS["w%02d" % i] = ("wv_%02d.png", (150 + 25 * i, 262, 25, 84))
by_index = {keys.index(k): v for k, v in PICS.items()}

skins = os.path.join(sys.argv[1], "Plugin Skins")
path = os.path.join(skins, "TUI.json")
tui = json.load(open(path))
local = tui["pageData"]["componentDefinitions"]["localComponentDefinitions"]
placed = [c for d in local if isinstance(d.get("value"), dict) for c in d["value"].get("componentsData", [])]

removed, used, per = set(), set(), {}
for c in placed:
    hs = c["bounds"].get("additionalInvalidatingHandles") or []
    m = re.match(r"IndexedEnabling/(\d+)/\d+/Parameter (\d+)$", hs[0]) if len(hs) == 1 else None
    if c["componentData"]["type"] != "Image" or not m or int(m.group(2)) not in by_index:
        continue
    pattern, (x, y, w, h) = by_index[int(m.group(2))]
    f = pattern % int(m.group(1))
    if not os.path.exists(os.path.join(ART, f)):
        sys.exit("skin_post: art/%s is missing (art/make_wave_art.py, art/make_wave_images.py)" % f)
    removed.add(c["componentData"]["data"]["image"])
    c["componentData"]["data"]["image"] = f
    c["bounds"]["bounds"] = "%d %d %d %d" % (x, y - Y, w, h)
    used.add(f)
    per[int(m.group(2))] = per.get(int(m.group(2)), 0) + 1

want = {i: len(next(p for p in json.load(open(os.path.join(HERE, "params.json")))["params"] if p["key"] == keys[i])["options"])
        for i in by_index}
for i, n in want.items():
    if per.get(i, 0) != n:
        sys.exit("skin_post: %s: expected %d picture images, found %d" % (keys[i], n, per.get(i, 0)))

json.dump(tui, open(path, "w"), indent=1)
for f in removed - used:
    if os.path.exists(os.path.join(skins, f)):
        os.remove(os.path.join(skins, f))
for f in used:
    shutil.copy(os.path.join(ART, f), os.path.join(skins, f))
print("skin_post: %d transparent picture images (%d pictures)" % (sum(per.values()), len(per)))
