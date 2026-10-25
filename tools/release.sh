#!/bin/sh
# Builds Cinematic Noir's release zip from the last device build (the toolkit's build_port.sh), catalog-ready:
#   sh tools/release.sh <version>        (from ports/noir, in a python:3 container with the repo mounted; tools/release.ps1)
# 1. the preset files (tools/make_browser.py), 2. the zip (tools/release.py) with the sounds, the User folder, the licences
# and one Presets/<Category> folder per category, 3. tools/patch_release.py (the installer fills in the subfolders),
# 4. tools/catalog_check.py.
set -e
V="$1"
[ -n "$V" ] || { echo "usage: sh tools/release.sh <version>"; exit 2; }
pip install -q pillow >/dev/null 2>&1 || true
python3 tools/make_browser.py
EXTRA=""
for c in Keys Strings Choir Brass Pads Synth Texture Leads; do EXTRA="$EXTRA --extra build/xpl/$c:Presets/$c"; done
python3 ../../tools/release.py --so build/noir.so --skin "build/skin/Milliano - VST - Cinematic Noir" \
    --entry build/pluginlist-entry.xml --version "$V" \
    --extra build/rom:rom --extra User-README.txt:User/README.txt --extra LICENSE:LICENSE.txt \
    --extra THIRD-PARTY-NOTICES.md:THIRD-PARTY-NOTICES.md $EXTRA --user-data User \
    --repo guerrillamilliano/CinematicNoir --license MIT --id cinematic-noir \
    --about "A dark, gritty boom-bap and crime-rap rompler: 33 software-modelled sounds, tape, vinyl, M/S, squash and mood FX, 204 presets." \
    -o dist
python3 tools/patch_release.py "dist/Cinematic-Noir-$V-mpc-armv7.zip"
python3 ../../tools/catalog_check.py "dist/Cinematic-Noir-$V-mpc-armv7.zip" --catalog --expect-id cinematic-noir \
    --expect-repo guerrillamilliano/CinematicNoir
