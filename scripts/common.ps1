# Shared helpers for install.ps1 / uninstall.ps1.

function Resolve-GameDir([string]$GameDir) {
    if (-not $GameDir) {
        $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
        if (-not $steam) { throw "Steam not found; pass -GameDir" }
        $libs = @($steam -replace '/', '\')
        $vdf = Join-Path $libs[0] 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            $libs += Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' |
                ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }
        }
        foreach ($lib in $libs | Select-Object -Unique) {
            $candidate = Join-Path $lib 'steamapps\common\Penumbra Overture\redist'
            if (Test-Path (Join-Path $candidate 'Penumbra.exe')) { $GameDir = $candidate; break }
        }
        if (-not $GameDir) { throw "Penumbra Overture not found in any Steam library; pass -GameDir" }
    }
    if (-not (Test-Path (Join-Path $GameDir 'Penumbra.exe'))) { throw "Penumbra.exe not found in $GameDir" }
    return (Resolve-Path $GameDir).Path
}

function Assert-GameClosed {
    if (Get-Process Penumbra -ErrorAction SilentlyContinue) { throw "Penumbra is running; close it first." }
}

# The fix DLL forwards to SDL_image_orig; the real SDL_image.dll never mentions that name.
function Test-IsFixDll([string]$Path) {
    if (-not (Test-Path $Path)) { return $false }
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($Path))
    return $text.Contains('SDL_image_orig')
}
