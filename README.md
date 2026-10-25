# Cinematic Noir

A dark, gritty rompler for the **Akai MPC** (MPC OS 3.x, armv7: MPC Live II, One, X and similar), built for boom-bap
and crime-rap production: menacing pianos, horror strings, opera choirs, mafioso brass, ominous pads and noir leads,
all dirtied up by tape, vinyl and crush. The sounds are modelled in software (no recordings) and printed through a
tube-and-tape stage.

- **33 sounds in 8 categories:** Keys, Strings, Choir, Brass, Pads, Synth, Texture and Leads, from a dark detuned grand
  and a saloon upright to a theremin, a noir whistle and a snarling analog lead.
- **204 presets**, grouped by category in the plugin's screen, in MPC's PRESET menu ("Keys: Basement Grand") and as
  preset files in folders (Sounds > INSTRUMENTS > Cinematic Noir).
- **Cinematic / Dark / Gritty modes:** one tap switches to a different sound of that mood in the same category, with
  its own settings.
- **A hardware-style screen** with the sound's waveform (lit while it plays, with a playhead and a TRIG lamp), its
  character and details, and what is playing.
- **Four pages, 16 Q-Links each:**
  - **PLAY:** Cutoff, Tape Drive, Drift (wow and flutter), Space, Attack, Crush, Sub Weight, Volume, Resonance,
    Release, Chop, Stutter, Key-Off noise, Cracked Vinyl, Width and Squash.
  - **TUNE:** transpose, fine tune, glide, bend range, the envelope, mono or poly, sample start, spread, velocity,
    octave drop, speed (45, 33, half) and the sound browser.
  - **MOOD:** Tilt (dark to bright), Warmth, Air, Shadow and Shimmer (ghost tails an octave down or up), Noir Echo,
    Ensemble and Pulse.
  - **FX:** Cracked Vinyl (dust, static, scratch), M/S stereo, the Spit & Grit compressor and a Tape Filter.
- **Your own sounds:** drop WAVs or multisample folders into the plugin's `User` folder (see `User-README.txt`).

## Install

Download `Cinematic-Noir-<version>-mpc-armv7.zip` from the [Releases](../../releases) page. Installing needs root
access to the MPC (SSH), as every community MPC plugin does, and about 170 MB free.

Copy the zip to the MPC (for example with `scp`), then on the MPC as root:

```sh
unzip Cinematic-Noir-0.7.1-mpc-armv7.zip && cd Cinematic-Noir-0.7.1
sha256sum -c SHA256SUMS   # optional: check the files
sh install.sh             # installs to the internal drive (/sdcard/Synths); restarts MPC, so save your project first
```

To install on an external drive instead, add `-t <its Synths folder>` (for example `-t /media/SSD/Synths`); that needs a
drive-exec patch, such as "Auto exec" from the mpc-vst-plugins patches. To remove it: `sh uninstall.sh` from the same
folder, with the same `-t` if you used one. Updating keeps the `User` folder. `INSTALL.md` in the zip has the details.

Then add **Cinematic Noir** to a track from the plugin browser.

## Build

Cinematic Noir is a port for [sd88me/mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) (MIT), which provides
the VST2 wrapper, skin builder and toolchain. It is not included here.

1. Clone mpc-vst-plugins and copy this repo's files (except `windows/`) into `ports/noir/`.
2. Render the sounds, presets and waveform pictures: `tools/make_rom.ps1` (Docker), or by hand: compile and run
   `tools/render_rom.c` (`render_rom build/rom catalogue.json`), then `python3 tools/gen_presets.py` and
   `python3 art/make_wave_images.py`. `art/make_wave_art.py` draws the playhead and lamp.
3. Build: `tools/build_port.sh ports/noir/vst.json` from the repo root. On Windows with Docker Desktop only, use
   `windows/mpc-build.ps1` (build the helper image first: `docker build -t mpc-builder windows/mpc-builder`).
4. Test: `tools/test_port.sh ports/noir/vst.json` (the toolkit's host test) and `test/demo.ps1 all` (loads every
   preset, checks the effects and the mode switching, and writes audition WAVs to `build/demo/`).
5. Release: `tools/release.ps1 <version>` (or `sh tools/release.sh <version>`).

| Path | What |
|---|---|
| `src/noir.c` | the engine: sample playback, the FX chain, presets, the screen's readouts and waveform |
| `src/presets.h` | the presets (written by `tools/gen_presets.py`) |
| `tools/render_rom.c` | the sound models; its table lists every sound, its range, modes and description |
| `tools/gen_presets.py` | the presets: each sound times a treatment of the FX, with per-category and per-mode characters |
| `tools/make_browser.py` | MPC preset files in folders, and the Instruments-browser tile |
| `tools/patch_release.py` | lets the installer fill in the presets in their folders |
| `layout.conf`, `art/` | the skin: layout, stylesheet, screen artwork, waveform pictures |
| `skin_post.py` | makes the stacked waveform pictures transparent after the build |
| `test/` | `demo.c` (load, effect and mode tests, audition WAVs), screen mockups |

## Licence

MIT, see [LICENSE](LICENSE). Third-party parts: [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
