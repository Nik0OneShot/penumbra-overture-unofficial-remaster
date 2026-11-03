# Removes the Penumbra Ultrawide fix and restores the original SDL_image.dll.
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1 [-GameDir <...\Penumbra Overture\redist>]
param([string]$GameDir)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\scripts\common.ps1"

$GameDir = Resolve-GameDir $GameDir
Assert-GameClosed

$dll = Join-Path $GameDir 'SDL_image.dll'
$orig = Join-Path $GameDir 'SDL_image_orig.dll'

if (-not (Test-Path $orig)) {
    if (Test-IsFixDll $dll) { throw "SDL_image_orig.dll is missing. Use Steam > Verify integrity of game files to restore SDL_image.dll." }
    Write-Host "The fix isn't installed in $GameDir"
    return
}
if (Test-IsFixDll $dll) { Remove-Item $dll }
if (-not (Test-Path $dll)) { Rename-Item $orig 'SDL_image.dll' }
foreach ($f in 'PenumbraUltrawide.ini', 'PenumbraUltrawide.log') {
    $p = Join-Path $GameDir $f
    if (Test-Path $p) { Remove-Item $p }
}
Write-Host "Removed the fix from $GameDir"
