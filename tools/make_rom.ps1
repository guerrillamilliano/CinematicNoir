# Renders Cinematic Noir's built-in sounds into ports/noir/build/rom and writes ports/noir/catalogue.json, then
# regenerates src/presets.h from it and draws each sound's waveform picture (art/wimg_NN.png).
# Docker Desktop only (gcc:11 and python:3.11-slim images).
#   .\make_rom.ps1
$port = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
docker run --rm -v "${port}:/w" -w /w gcc:11 sh -c "mkdir -p build && gcc -O2 -o build/render_rom tools/render_rom.c -lm && rm -rf build/rom && ./build/render_rom build/rom catalogue.json"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
docker run --rm -v "${port}:/w" -w /w python:3.11-slim sh -c "python3 -I tools/gen_presets.py && pip install -q pillow >/dev/null 2>&1 && python3 art/make_wave_images.py"
exit $LASTEXITCODE
