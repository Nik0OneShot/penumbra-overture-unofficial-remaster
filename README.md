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
- Things at the far left and right of a wide view no longer turn black. The engine picks its
  stencil-shadow method from a box around the camera that only covers about a 4:3 view, so
  shadows went wrong at the screen edges. On very wide screens (about 2.6:1 and up, e.g. 32:9),
  fog culling no longer hides everything in foggy levels.
- **High framerate:** the game's logic (player, camera, physics, scripts) normally updates
  60 times a second, and frames are only drawn after an update, so it never looks smoother
  than 60 FPS. The fix runs the logic at your monitor's refresh rate (or the `LogicRate` you
  set), so motion is genuinely smooth at 120/144/165+ Hz. See below for what that involves.
- **Environmental audio crash (dormant).** The sound library shared with Black Plague can crash
  in `SDL.dll` from its sound thread when environmental audio (EFX) is on. An effect slot only
  creates its lock when a flag is set, and that flag isn't set yet when the slots are made.
  Overture never turns EFX on, so this can't happen in normal play. The 2-byte fix is applied
  anyway, matching the Black Plague remaster.
- **Crash reports:** if the game crashes in its own code or DLLs, `PenumbraUltrawide.log`
  records where (module and offset), the registers and the call stack, which makes it
  possible to track the cause down.

## High framerate

Running the logic faster isn't enough on its own. Some code was written for exactly 60 updates
a second: it counts updates, or applies a push for a single update. Left alone, at 165 Hz you
would jump a third as high, doors would feel nearly 3× heavier, and steam would kill nearly 3×
faster. The fix compensates each of these so the game plays as it does at 60 Hz:

- **Player:** jump take-off; dragging doors, drawers, levers and valves; push/pull kicks and
  shoves; the screen-edge turn while dragging; the ground grace period (ledge jumps, air
  steering); the "standing still" check for hiding in shadow; camera and hand smoothing; slope
  sliding.
- **Combat:** knockback from enemy hits and explosions; explosion pushes on objects; the
  spider's leap; the hammer/pickaxe flick threshold; spin on hit and thrown objects; debris
  from broken objects.
- **Enemies:** path following; stair stepping; mesh smoothing; how the worm's body trails its
  head; how long footstep sounds stay audible to them.
- **Physics:** how long props take to fall asleep after coming to rest; swing-door braking;
  the PID timing of every joint controller in the entity files (valve and wheel slow-down,
  levers, doors that drive themselves); how long a dragged door, drawer or lever keeps moving
  before it brakes.
- **Level scripts:** steam, acid, freezing-water and closing-door damage; the refinery fan; the
  storage-room crane; the worm-nest valve. These are adjusted as the scripts load; the files on
  disk are untouched.

The rate is set by `LogicRate` in `PenumbraUltrawide.ini`: `0` matches your monitor (default),
and `60` gives the original behaviour. With "Limit FPS" on (the default), the game draws one
frame per update, so the framerate equals the logic rate.

Minor differences that remain: the fear "image trail" effect and screen shake are a little
quicker, scrape and creak sounds start a little more readily, and sliding down slopes that are
too steep to stand on is slower. Physics contacts are also a little stiffer at high rates, so a
lever or wheel that only reached its end because something sank a few millimetres into
something else could stop just short. If anything feels off, set `LogicRate=60` to compare.

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
| Wide-view culling | `cFrustum::SetViewProjMatrix` sizes `mOriginBV` (used to choose Z-fail shadows) to contain the whole near plane for the actual FOV and aspect, instead of `near*2`. The fog-culling cosine in `cRenderer3D::UpdateRenderList` falls back to the exact view-corner value where `cos(fov*aspect/2)` stops being valid. |
| Bug fixes & crash reports | OALWrapper's `cOAL_EffectSlot` constructor skips `SDL_CreateMutex` unless the EFX manager's threaded flag is set, which happens only after the slots are built. The `je` is NOPed so the mutex is always created. A vectored exception handler logs fatal faults in game-folder modules: module+offset, the faulting address, registers, and game-module return addresses on the stack. It ignores the DLL's own guarded probes and never changes how an exception is handled. |
| Doorway margin | Rooms are only drawn when a doorway (portal quad) into them is in view. Some room meshes reach past their quad, so at 21:9 you could see into a room whose quad was just off screen, and it showed black. The portal test in `cSectorVisibilityContainer::SearchSector` is retried with the frustum's side and near planes pushed out by `DoorwayMargin` metres (default 2), then restored. Objects are still culled with the exact view. Rescued doorways are logged. |
| Logic rate | `cLogicTimer::GetStepSize` is replaced. On the first call (during startup) it sets the timer's milliseconds-per-tick to `1000 / LogicRate`. The tick schedule and every step size, including ones the game caches at startup, follow from that one field. |
| Tick compensations (`HighFramerate.cpp`) | Each 60 Hz assumption becomes a constant scaled by rate/60 or 60/rate. Joint controllers loaded from `.ent` files get their `IntegralSize` scaled in `cEntityLoader_Object::LoadController` (0 stays 0). Shared x87 constants are handled by pointing the one instruction at a scaled copy. Tick counts are patched as immediates. One-tick forces and torques go through small stubs that pass a scaled copy of the vector. Newton's freeze-step count is scaled at its import. Level scripts are rewritten in the buffer that `cSqScript::LoadCharBuffer` returns. If any of these sites doesn't match, the game stays at 60 Hz. |

The DLL patches at the game's first `SDL_Init` call, hooked through the import table in
`DllMain`. That is before any game object is built, so values stored by constructors are
covered. The first `IMG_Load` is the fallback.

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
