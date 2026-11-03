# Installs the Penumbra Ultrawide fix into the Steam copy of Penumbra: Overture.
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-GameDir <...\Penumbra Overture\redist>]
param([string]$GameDir)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\scripts\common.ps1"

$GameDir = Resolve-GameDir $GameDir
Assert-GameClosed

$dll = Join-Path $GameDir 'SDL_image.dll'
$orig = Join-Path $GameDir 'SDL_image_orig.dll'
$ini = Join-Path $GameDir 'PenumbraUltrawide.ini'

if (Test-Path $orig) {
    Write-Host "Already installed once; updating the fix DLL."
} elseif (Test-IsFixDll $dll) {
    throw "SDL_image.dll is already the fix but SDL_image_orig.dll is missing. Verify the game files in Steam, then run this again."
} else {
    Rename-Item $dll 'SDL_image_orig.dll'
    Write-Host "Backed up the original SDL_image.dll as SDL_image_orig.dll"
}
Copy-Item "$PSScriptRoot\dist\SDL_image.dll" $dll -Force
if (-not (Test-Path $ini)) { Copy-Item "$PSScriptRoot\dist\PenumbraUltrawide.ini" $ini }
Write-Host "Installed to $GameDir"
Write-Host "After launching, PenumbraUltrawide.log in that folder says whether the patches applied."
