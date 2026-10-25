#!/usr/bin/env python3
"""Fixes up a release zip from tools/release.py for Cinematic Noir's preset folders:
the generated install.sh fills in the %payload-path% placeholder only for presets directly in Presets/, and these sit in
Presets/<Category>/<Instrument>/. The loop is rewritten to walk the whole tree (BusyBox find), and SHA256SUMS is updated
for the changed install.sh. Fails if the loop it rewrites is not there. Other files and their modes are kept.
    python3 tools/patch_release.py dist/<zip>"""
import hashlib
import re
import sys
import zipfile

OLD = 'for f in "$STAGE"/Presets/*.xpl; do'
NEW = "find \"$STAGE/Presets\" -name '*.xpl' 2>/dev/null | while IFS= read -r f; do   # presets sit in category folders"

path = sys.argv[1]
zin = zipfile.ZipFile(path)
items = [(i, zin.read(i.filename)) for i in zin.infolist()]
zin.close()

out, installer = [], None
for info, data in items:
    if info.filename.endswith("/install.sh"):
        text = data.decode()
        if OLD not in text:
            sys.exit("patch_release: the preset loop is not in %s" % info.filename)
        data = text.replace(OLD, NEW).encode()
        installer = (info.filename, hashlib.sha256(data).hexdigest())
    out.append((info, data))
if not installer:
    sys.exit("patch_release: no install.sh in %s" % path)

root = installer[0].rsplit("/", 1)[0] + "/"
for k, (info, data) in enumerate(out):
    if info.filename == root + "SHA256SUMS":
        sums = re.sub(r"^[0-9a-f]{64}  install\.sh$", installer[1] + "  install.sh", data.decode(), flags=re.M)
        out[k] = (info, sums.encode())

with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
    for info, data in out:
        z.writestr(info, data)   # the same ZipInfo: name, date and unix modes (external_attr) kept
print("patch_release: %s: the installer fills in presets in every Presets/ folder" % path)
