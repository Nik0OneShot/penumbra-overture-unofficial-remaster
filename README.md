# Penumbra: Overture ultrawide fix

Makes the Steam version of Penumbra: Overture (build 459398) look right on widescreen and
ultrawide displays such as 3440x1440.

## What it fixes

The 3D view already handles wide screens: it keeps a 70° vertical FOV and shows more at the
sides. Everything 2D, though, is laid out on a fixed 800x600 canvas and stretched to fill the
screen, about 1.8× too wide at 21:9. With the fix installed:

- Menus, HUD, text, inventory, notebook and loading screens keep their 4:3 shape, centred,
  with black bars at the sides.
- Full-screen effects (fades, flashes, damage, the sanity noise, letterbox bars) still cover
  the whole screen.
- The in-world cursor (grabbing, using items) still lines up with what you point at.
- Horizontal mouse speed matches vertical again. At 21:9 it was about 1.8× slower, so it
  will feel faster horizontally than before.
- Options > Graphics offers modern resolutions, including your desktop resolution. Without
  the fix the list stops at 1920x1200.

## Requirements

- Windows, and the **Steam** version of Penumbra: Overture. Other versions (GOG, retail
  discs) have different code, so the fix detects them and does nothing.
- No game files are included or modified. The fix only adds a DLL next to the game.

## Install

Download this repository (**Code > Download ZIP**) and extract it. `dist\` holds the prebuilt
DLL. Close the game, then from the extracted folder run:

```bash
powershell -ExecutionPolicy Bypass -File install.ps1
```

The script finds the game through your Steam libraries (pass `-GameDir` to point it
somewhere else). It renames the game's `SDL_image.dll` to `SDL_image_orig.dll` and puts the
fix in its place. Then set the resolution in Options > Graphics, or edit
`Documents\Penumbra Overture\Episode1\settings.cfg` (`<Screen Width="3440" Height="1440" .../>`).

Each launch writes `PenumbraUltrawide.log` next to `Penumbra.exe`, saying whether the patches
applied. `PenumbraUltrawide.ini` there turns each part on or off.

## Uninstall

```bash
powershell -ExecutionPolicy Bypass -File uninstall.ps1
```

Steam's "Verify integrity of game files" also puts the original DLL back, which removes the
fix. Run `install.ps1` again afterwards if you want it back.

## How it works

`Penumbra.exe` is wrapped in Steam DRM, so its code is encrypted on disk and can't be patched
there. Instead the fix ships as a stand-in `SDL_image.dll` that forwards everything to the real
one. On the game's first image load, after Steam has decrypted the game, it patches the code
in memory:

| Part | Patch |
|---|---|
| UI projection | `SetOrthoProjection` (vtable) returns a centred 4:3 projection, but only for the UI draw passes (`DrawAll`, `cGraphicsHelper::DrawTexture`, intro). Post effects keep the full-screen one. |
| Overlays | `cGraphicsDrawer::DrawGfxObject` is detoured: anything spanning the full 800-unit width is stretched to the real screen edges. The noise tiles and intro bars are redirected at their call sites. |
| Cursor | The crosshair and held-item draws convert the cursor centre from stretched screen space (the space 3D picking uses) into the centred layout. The inventory, notebook, keypad and death menu reuse the crosshair as their mouse pointer (state `Pointer`, already in layout space), so that state is drawn unconverted. A thunk reads the `cPlayer` from `esi` at the call site to check the state. |
| Mouse | `cMouseSDL::Update` (vtable) scales horizontal relative motion by the aspect factor. The four `afVal * 800` cursor multipliers are pointed at a compensating value. |
| Resolutions | The 17-entry `gvResolutions` table is rewritten in place. |

Before patching, the DLL checks the exact bytes at every patch site. On any other build it
logs the mismatch and leaves the game untouched.

Engine reference: Frictional's open-source release of the game and engine
([PenumbraOverture](https://github.com/FrictionalGames/PenumbraOverture),
[HPL1Engine](https://github.com/FrictionalGames/HPL1Engine)), which matches this build closely.

## Building and testing

- `src\build.bat` builds `dist\SDL_image.dll` with the Visual Studio 2022 Build Tools (x86).
- `tests\run_test.bat <dump> <work dir> [game dir]` maps a memory snapshot of the running game
  at its real address, lets the DLL patch it, and drives every hook with fake objects
  (projection, overlay stretching, cursor mapping, mouse scaling, resolution list). The game
  dir (default `game\redist`) supplies the real `SDL_image.dll` and its dependencies. The
  snapshot holds the game's code, so it isn't kept in this repo. Make one with
  `python -I tests\dumpmem.py penumbra_mem.bin` (read-only) while the game sits at the main menu.

## Credits

Not affiliated with Frictional Games. Penumbra: Overture and its files belong to their owners.
