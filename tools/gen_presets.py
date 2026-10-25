#!/usr/bin/env python3
"""Writes Cinematic Noir's presets into the engine (src/presets.h) from catalogue.json (written by render_rom).
Each preset is a sound plus a treatment of the FX chain, stored as a state string the engine applies ("key=value;...",
options as indices, the sound by name as sname=). Presets are grouped by category (the sound name's prefix) in CATS
order; the engine browses them by category and lists them in MPC's PRESET menu as "Category: Name" (vst.json
"programs"). The first preset is the plugin's default (params.json defaults). Also sets params.json's preset range to
the preset count. Run from the port folder: python3 tools/gen_presets.py"""
import json
import os
import re

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODES = ["CINEMATIC", "DARK", "GRITTY"]
CATS = ["Keys", "Strings", "Choir", "Brass", "Pads", "Synth", "Texture", "Leads"]   # must match params.json "category"
ENGINE_SET = {"preset", "category", "sound", "wave_head", "trig", "wave_img"} | {"w%02d" % i for i in range(40)}
# not in a preset: the engine sets them (the sound comes as sname; the rest are the screen's waveform)

params = json.load(open(os.path.join(HERE, "params.json")))["params"]
PBYKEY = {p["key"]: p for p in params}
BASE = {}
for p in params:
    if p.get("momentary") or p.get("type") == "readout" or p["key"] in ENGINE_SET:
        continue
    d = p.get("default", 0)
    BASE[p["key"]] = p["options"][d] if "options" in p else d

# per category: changes to the defaults every preset of it starts from
CAT_BASE = {"Pads": {"attack": 0.42, "release": 0.62, "space": 40, "pan_spread": 45},
            "Strings": {"pan_spread": 25},
            "Leads": {"voice_mode": "MONO", "glide": 0.3, "echo_mix": 14, "echo_time": 0.52, "echo_feedback": 30}}

# per mode: the character a preset of that mode starts from (after its category, before its treatments), so switching
# modes changes the knobs as well as the sound. DARK is the defaults themselves (params.json), so preset 0 matches them.
MODE_BASE = {
    "CINEMATIC": {"space": 48, "space_type": "PLATE", "width": 145, "tail_warble": 45, "drive": 18, "cutoff": 0.85,
                  "tape_highcut": 1.0, "vinyl": 15, "tone_tilt": 56, "shadow": 0, "shimmer": 22, "tone_air": 30, "ensemble": 25},
    "DARK": {},
    "GRITTY": {"drive": 58, "crush": 0.22, "squash": 45, "width": 70, "cutoff": 0.75, "tape_lowcut": 0.15,
               "tape_highcut": 0.9, "vinyl": 30, "space": 20, "tone_tilt": 48, "shadow": 0, "tone_warmth": 35},
}

# treatments: overrides on the defaults
T = {
    "dusty":     {"drive": 40, "drift": 25, "vinyl": 45, "cutoff": 0.72, "tape_highcut": 0.72},
    "basement":  {"space_type": "ROOM", "space": 35, "drive": 45, "cutoff": 0.64, "squash": 40, "vinyl": 25, "tape_lowcut": 0.22, "tape_bump": 45},
    "warehouse": {"space_type": "WAREHOUSE", "space": 55, "drift": 20, "width": 140},
    "plate":     {"space_type": "PLATE", "space": 45},
    "sp12":      {"crush": 0.38, "drive": 50, "cutoff": 0.68, "vinyl": 30, "squash": 30, "tape_lowcut": 0.18},
    "lofi8":     {"crush": 0.65, "cutoff": 0.56, "drive": 40},
    "muffled":   {"cutoff": 0.46, "reso": 30, "drive": 35, "vinyl": 35, "tape_highcut": 0.5},
    "warped":    {"drift": 70, "drive": 45, "vinyl": 40, "speed": "33", "tail_warble": 60, "echo_mix": 18, "echo_time": 0.55},
    "halfspeed": {"speed": "HALF", "drift": 30, "vinyl": 35, "cutoff": 0.62, "tape_highcut": 0.62, "tape_bump": 30},
    "stab":      {"chop": 0.55, "attack": 0.0, "release": 0.15, "squash": 50, "drive": 55, "squash_style": "SPIT"},
    "stutter":   {"stutter": "1/16", "chop": 0.4},
    "swell":     {"attack": 0.62, "release": 0.62, "space": 50},
    "subweight": {"sub": 60, "cutoff": 0.72},
    "octdrop":   {"octdrop": "DOUBLE", "drive": 45},
    "sqdrop":    {"octdrop": "SQUARE"},
    "wide":      {"width": 175, "space": 45, "tail_warble": 45},
    "mono":      {"width": 40, "squash": 45, "drive": 55},
    "squashed":  {"squash": 75, "space": 40, "squash_style": "GLUE"},
    "cracked":   {"vinyl": 80, "drift": 35},
    "keyoff":    {"keyoff": 60},
    "monoglide": {"voice_mode": "MONO", "glide": 0.38},
}

# per sound (catalogue name): (preset name, treatments, mode or None = the sound's first mode)
P = {
    "Keys - Dark Grand": [("Dark Grand", [], "DARK"), ("Basement Grand", ["basement", "keyoff"], "GRITTY"),
        ("Dusty Ledger Keys", ["dusty", "keyoff"], None), ("Twelve-Bit Grand", ["sp12"], "GRITTY"),
        ("Halfspeed Hymn", ["halfspeed"], None), ("Muffled Confession", ["muffled", "keyoff"], None),
        ("Grand Jury Stabs", ["stab"], "GRITTY")],
    "Keys - Detuned Grand": [("Detuned Grand", [], None), ("Warped Waltz", ["warped"], None),
        ("Cold Parlor", ["plate", "keyoff"], None), ("Wide Detune Noir", ["wide"], "CINEMATIC"),
        ("Cracked Lacquer Keys", ["cracked"], None), ("Swelling Grand", ["swell"], "CINEMATIC"),
        ("Pitchdown Grand", ["octdrop"], None)],
    "Keys - Saloon Upright": [("Saloon Upright", [], None), ("Back Room Upright", ["basement"], None),
        ("Honky Crime Keys", ["sp12"], None), ("Squashed Upright", ["squashed", "mono"], None),
        ("Upright Stutter", ["stutter"], None), ("Smoke Shop Upright", ["dusty"], "DARK"),
        ("Sub Upright", ["subweight", "mono"], None)],
    "Keys - Smoky Rhodes": [("Smoky Rhodes", [], None), ("Velvet Rope Rhodes", ["dusty"], None),
        ("Rhodes Halfspeed", ["halfspeed"], None), ("Late Night Rhodes", ["plate", "wide"], None),
        ("Crushed Rhodes", ["lofi8"], None), ("Rhodes Muffle", ["muffled"], None)],
    "Keys - Velvet Wurli": [("Velvet Wurli", [], None), ("Wurli Grit", ["sp12", "mono"], "GRITTY"),
        ("Warped Wurli", ["warped"], None), ("Wurli Basement", ["basement"], None),
        ("Wurli Stab", ["stab"], "GRITTY"), ("Wurli Sub", ["subweight"], None)],
    "Keys - Funeral Organ": [("Funeral Organ", [], None), ("Chapel Of Sins", ["warehouse"], None),
        ("Organ Drift", ["warped"], "DARK"), ("Organ 12-Bit", ["sp12"], "DARK"),
        ("Organ Swell", ["swell"], None), ("Low Mass Organ", ["octdrop"], "DARK")],
    "Strings - Horror Swell": [("Horror Swell", [], None), ("Scream Queen Violins", ["wide", "plate"], None),
        ("Night Shift Swell", ["muffled"], None), ("Swell At 33", ["warped"], None),
        ("Crushed Swell", ["sp12"], None), ("Warehouse Horror", ["warehouse"], None),
        ("Swell Stutter", ["stutter"], None)],
    "Strings - Weeping Section": [("Weeping Section", [], None), ("Funeral Strings", ["warehouse"], None),
        ("Dusty Lament", ["dusty"], None), ("Weeping Halfspeed", ["halfspeed"], None),
        ("Wide Tears", ["wide"], None), ("Weeping Muffle", ["muffled"], None),
        ("Cracked Requiem", ["cracked"], None)],
    "Strings - Low Cellos": [("Low Cellos", [], None), ("Cello Menace", ["mono", "squashed"], "GRITTY"),
        ("Cello Sub", ["subweight"], "DARK"), ("Cello 12-Bit", ["sp12"], "GRITTY"),
        ("Basement Cellos", ["basement"], "GRITTY"), ("Cello Stab", ["stab"], "GRITTY")],
    "Strings - Tremolo Panic": [("Tremolo Panic", [], None), ("Panic Room", ["basement"], None),
        ("Panic Warehouse", ["warehouse"], None), ("Panic Crushed", ["lofi8"], "GRITTY"),
        ("Panic Halfspeed", ["halfspeed"], None), ("Panic Wide", ["wide"], None)],
    "Strings - Pizzicato Plucks": [("Pizzicato Plucks", [], None), ("Creeping Pizz", ["muffled"], "DARK"),
        ("Pizz 12-Bit", ["sp12"], None), ("Pizz Stutter", ["stutter"], None),
        ("Pizz Drop", ["octdrop"], "DARK"), ("Cracked Pizz", ["cracked"], None)],
    "Choir - Opera Aah": [("Opera Aah", [], None), ("Cathedral Aah", ["warehouse"], None),
        ("Aah Halfspeed", ["halfspeed"], None), ("Crushed Choir", ["sp12"], "GRITTY"),
        ("Muffled Mass", ["muffled"], "DARK"), ("Wide Opera", ["wide"], None), ("Choir Swell", ["swell"], None)],
    "Choir - Dark Ooh": [("Dark Ooh", [], None), ("Ghost Ooh", ["warped"], "DARK"),
        ("Ooh Basement", ["basement"], "DARK"), ("Ooh Halfspeed", ["halfspeed"], None),
        ("Ooh Cracked", ["cracked"], "DARK"), ("Ooh Wide", ["wide"], None)],
    "Choir - Opera Stab": [("Opera Stab", [], None), ("Stab 12-Bit", ["sp12"], "GRITTY"),
        ("Stab Squash", ["squashed"], "GRITTY"), ("Stab Drop", ["octdrop"], "GRITTY"),
        ("Stab Stutter", ["stutter"], None), ("Stab Warehouse", ["warehouse"], None)],
    "Brass - Mafioso Stab": [("Mafioso Stab", [], None), ("Made Man Brass", ["mono", "squashed"], None),
        ("Brass 12-Bit", ["sp12"], None), ("Brass Drop", ["octdrop"], None),
        ("Brass Stutter", ["stutter"], None), ("Brass Basement", ["basement"], None)],
    "Brass - Muted Trumpet": [("Muted Trumpet", [], None), ("Trumpet Warble", ["warped"], "CINEMATIC"),
        ("Trumpet After Hours", ["plate"], "DARK"), ("Dusty Trumpet", ["dusty"], "DARK"),
        ("Trumpet Halfspeed", ["halfspeed"], None), ("Trumpet In The Fog", ["muffled"], "DARK")],
    "Brass - Dark Horns": [("Dark Horns", [], None), ("Horns Of Doom", ["warehouse", "subweight"], None),
        ("Horn Swell", ["swell"], None), ("Horns 12-Bit", ["sp12"], "GRITTY"),
        ("Horn Stab", ["stab"], "GRITTY"), ("Wide Horns", ["wide"], None)],
    "Brass - Smoke Sax": [("Smoke Sax", [], None), ("Sax Back Room", ["basement"], None),
        ("Warped Sax", ["warped"], None), ("Sax 12-Bit", ["sp12"], "GRITTY"),
        ("Sax Halfspeed", ["halfspeed"], None), ("Dusty Sax", ["dusty"], None)],
    "Pads - Ominous Choir Pad": [("Ominous Choir Pad", [], None), ("Seance", ["warehouse"], None),
        ("Choir Pad Halfspeed", ["halfspeed"], None), ("Cracked Choir Pad", ["cracked"], "DARK"),
        ("Wide Omen", ["wide"], None), ("Choir Pad 12-Bit", ["sp12"], "GRITTY")],
    "Pads - Cold War Pad": [("Cold War Pad", [], None), ("Interrogation Room", ["basement"], None),
        ("Cold War Warped", ["warped"], None), ("Cold War Crushed", ["lofi8"], "GRITTY"),
        ("Cold War Sub", ["subweight"], None), ("Bunker Squash", ["squashed"], "GRITTY")],
    "Pads - Blood Moon": [("Blood Moon", [], None), ("Blood Moon Warehouse", ["warehouse"], None),
        ("Blood Moon Muffle", ["muffled"], None), ("Blood Moon Swell", ["swell"], None),
        ("Blood Moon Wide", ["wide"], None), ("Blood Moon Cracked", ["cracked"], "DARK")],
    "Synth - Suspense Drone": [("Suspense Drone", [], None), ("Drone Warehouse", ["warehouse"], None),
        ("Drone Wide", ["wide"], None), ("Drone Crushed", ["lofi8"], "DARK"),
        ("Drone Cracked", ["cracked"], "DARK"), ("Drone Sub", ["subweight"], "DARK")],
    "Synth - Sub Rumble": [("Sub Rumble", ["monoglide"], None), ("Rumble Squash", ["squashed"], "GRITTY"),
        ("Rumble Drop", ["sqdrop"], None), ("Rumble Grit", ["sp12"], "GRITTY"),
        ("Rumble Muffle", ["muffled"], "CINEMATIC"), ("Rumble Stutter", ["stutter"], "GRITTY")],
    "Synth - Sinister Bass": [("Sinister Bass", ["monoglide"], None), ("Bass Grit", ["sp12", "mono"], None),
        ("Bass Drop", ["sqdrop", "monoglide"], None), ("Bass Muffle", ["muffled"], "DARK"),
        ("Bass Stab", ["stab"], None), ("Bass Squash", ["squashed"], None)],
    "Texture - Music Box": [("Music Box", [], None), ("Haunted Box", ["warped"], None),
        ("Box Halfspeed", ["halfspeed"], None), ("Cracked Box", ["cracked"], "GRITTY"),
        ("Box Warehouse", ["warehouse"], "CINEMATIC"), ("Box 8-Bit", ["lofi8"], "GRITTY")],
    "Texture - Ghost Glass": [("Ghost Glass", [], None), ("Glass Warehouse", ["warehouse"], None),
        ("Warped Glass", ["warped"], None), ("Glass Wide", ["wide"], None),
        ("Glass Crushed", ["lofi8"], "DARK"), ("Glass Swell", ["swell"], None)],
    "Texture - Vibes Noir": [("Vibes Noir", [], None), ("Vibes Back Room", ["basement"], None),
        ("Dusty Vibes", ["dusty"], None), ("Vibes Halfspeed", ["halfspeed"], None),
        ("Vibes Plate", ["plate"], None), ("Vibes 12-Bit", ["sp12"], "GRITTY")],
    "Leads - Theremin Ghost": [("Theremin Ghost", [], None), ("Haunted Theremin", ["warehouse"], None),
        ("Theremin Halfspeed", ["halfspeed"], None), ("Cracked Theremin", ["cracked"], "DARK"),
        ("Theremin Shimmer", ["wide"], None), ("Theremin Fog", ["muffled"], "DARK")],
    "Leads - Saw Detective": [("Saw Detective", [], None), ("Stakeout Lead", ["dusty"], None),
        ("Detective 12-Bit", ["sp12"], None), ("Interrogation Lead", ["basement"], None),
        ("Warped Detective", ["warped"], "DARK"), ("Chase Scene Lead", ["mono"], None)],
    "Leads - Noir Whistle": [("Noir Whistle", [], None), ("Desert Whistle", ["warehouse"], None),
        ("Whistle Halfspeed", ["halfspeed"], None), ("Dusty Whistle", ["dusty"], "GRITTY"),
        ("Whistle 12-Bit", ["sp12"], "GRITTY"), ("Whistle Cathedral", ["wide"], None)],
    "Leads - Square Specter": [("Square Specter", [], None), ("Specter Crushed", ["lofi8"], "GRITTY"),
        ("Specter Muffle", ["muffled"], None), ("Specter Stutter", ["stutter"], "GRITTY"),
        ("Cracked Specter", ["cracked"], None), ("Specter Basement", ["basement"], None)],
    "Leads - Moog Menace": [("Moog Menace", [], None), ("Menace Squash", ["squashed"], None),
        ("Menace Drop", ["octdrop"], None), ("Menace Grit", ["sp12"], None),
        ("Menace Warehouse", ["warehouse"], "CINEMATIC"), ("Menace Wide", ["wide"], "CINEMATIC")],
    "Leads - Glass Flute": [("Glass Flute", [], None), ("Flute Of Ashes", ["warehouse"], None),
        ("Flute Halfspeed", ["halfspeed"], None), ("Dusty Flute", ["dusty"], "DARK"),
        ("Flute Fog", ["muffled"], "DARK"), ("Flute Wide", ["wide"], None)],
}


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    cat = json.load(open(os.path.join(HERE, "catalogue.json")))["sounds"]
    def cat_of(name):
        c = name.split(" - ")[0]
        if c not in CATS:
            raise SystemExit("gen_presets: %r has no known category" % name)
        return CATS.index(c)
    sounds = sorted(cat, key=lambda s: cat_of(s["name"]))   # stable: catalogue order within a category
    out, seen = [], set()
    for s in sounds:
        name, ci = s["name"], cat_of(s["name"])
        if name not in P:
            raise SystemExit("gen_presets: no presets for %r" % name)
        for pname, treats, mode in P[name]:
            mode = mode or s["modes"].split(",")[0].upper()
            if mode.lower() not in s["modes"].split(","):
                raise SystemExit("gen_presets: %r is not listed in %s" % (name, mode))
            if pname in seen:
                raise SystemExit("gen_presets: duplicate preset name %r" % pname)
            seen.add(pname)
            v = dict(BASE)
            v.update(CAT_BASE.get(CATS[ci], {}))
            v.update(MODE_BASE[mode])
            for t in treats:
                v.update(T[t])
            v["mode"] = mode
            parts = ["mode=%d" % MODES.index(mode)]   # the mode first: it picks the list the sound is found in
            for k, x in v.items():
                if k == "mode":
                    continue
                p = PBYKEY[k]
                parts.append("%s=%s" % (k, p["options"].index(x) if "options" in p else x))
            parts.append("sname=" + name)
            out.append((pname, ci, MODES.index(mode), name, ";".join(parts) + ";"))
    if out[0][0] != "Dark Grand":
        raise SystemExit("gen_presets: the first preset must be the default sound")
    # a mode switch loads a preset of that mode in the same category with a different sound: every (category, mode) pair
    # needs presets of two sounds at least, or switching from one of them would keep the sound
    for ci, cn in enumerate(CATS):
        for mi, mn in enumerate(MODES):
            sounds = {o[3] for o in out if o[1] == ci and o[2] == mi}
            if len(sounds) < 2:
                raise SystemExit("gen_presets: %s has %s presets of %d sound(s); a mode switch needs two" % (cn, mn, len(sounds)))
    with open(os.path.join(HERE, "src", "presets.h"), "w") as f:
        f.write("/* WRITTEN BY tools/gen_presets.py from catalogue.json: edit that script, not this file. */\n")
        f.write("#define NOIR_NPRESETS %d\n" % len(out))
        f.write("static const struct { const char *name; int cat, mode; const char *sound, *state; } NOIR_PRESETS[NOIR_NPRESETS] = {\n")
        for n, c, m, snd, st in out:
            f.write("    { %s, %d, %d, %s, %s },\n" % (c_str(n), c, m, c_str(snd), c_str(st)))
        f.write("};\n")
    # the preset param's range is the preset count (MPC's PRESET menu lists every value)
    path = os.path.join(HERE, "params.json")
    txt = open(path).read()
    txt2 = re.sub(r'("key": "preset", "name": "Preset", "min": 0, "max": )\d+', r"\g<1>%d" % (len(out) - 1), txt)
    if txt2 != txt:
        open(path, "w").write(txt2)
    print("src/presets.h: %d presets in %d categories" % (len(out), len({o[1] for o in out})))


main()
