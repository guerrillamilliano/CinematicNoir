# Builds test/demo.c (under ASan/UBSan) and renders audition WAVs into ports/noir/build/demo from the engine's presets.
#   .\demo.ps1            one per sound (its first preset)
#   .\demo.ps1 all        every preset
# Needs build/rom (tools/make_rom.ps1) and Docker Desktop.
param([string]$Which = "first")
$port = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$repo = (Resolve-Path -LiteralPath (Join-Path $port "..\..")).Path
docker run --rm -v "${repo}:/r" -w /r/ports/noir gcc:11 sh -c "rm -rf build/demo && gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -I/r/wrapper -o build/demo_run test/demo.c -lm -lpthread && ./build/demo_run $Which"
exit $LASTEXITCODE
