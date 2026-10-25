# Builds the release zip (tools/release.sh) in Docker after a device build.
#   .\release.ps1 0.7.1
param([Parameter(Mandatory = $true)][string]$Version)
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..")).Path
docker run --rm -v "${repo}:/r" -w /r/ports/noir python:3.11-slim sh tools/release.sh $Version
exit $LASTEXITCODE
