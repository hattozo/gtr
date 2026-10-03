# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A passthrough mashup: Vanadium (a Roblox-compatible engine) plays a Roblox place as the **guest**. GTA V Legacy is the
**host**, in story mode. The two run side by side. The guest's character, bricks, tools and GUI are drawn into GTA's
picture, lit and shadowed like GTA's world, and what the tools do happens in GTA too. `MODDING_PLAN.md` is the design
document and a dated log of every batch of work and the reasons behind it. Read the relevant part before changing a
subsystem, and add to it when you finish one.

`MODDING_PLAN.md` started out targeting GTA V Enhanced, where ScriptHookV refused the game version. The working install
is **Legacy** (`D:\SteamLibrary\steamapps\common\Grand Theft Auto V`), which `host/gta/install.ps1` picks by default.

## Hard rules

- **Never edit or build in the user's own Vanadium checkout** (`..\vanadium`, beside this repo). The guest builds
  against a private clone in `vanadium/`, a git submodule of `github.com/noobwarrior-org/dieselnoob`, pinned at
  `492beca` (the commit before Vanadium's big "Added everything" change, which breaks the guest). Its working tree
  carries the patch below uncommitted, so the submodule shows as modified. Move the pin only when the user asks.
- **Engine changes go in `patches/vanadium-host-view.patch`**, applied to the clone. When the clone is updated from
  the user's Vanadium (another agent is changing its renderer and VehicleSeats there), the patch must be *rewritten*
  against the new code, not re-applied. It covers:
  - `Renderer::GetSceneResources` and `SetMainSceneDrawn(false)`;
  - the transparent-coverage pass: `PartPass::Coverage`, `fs_coverage` in part.wgsl, `SceneResources::CoveragePipeline`,
    and `SceneView::SetTransparentCoverage/SetSkyDrawn/GetCoverageView/DrawCoverage`.
- **Check that GTA is closed** (`tasklist | grep -i GTA5`) before:
  - running any test or tool that connects to the guest as the host (anything that sends `origin`, `ground`, `spawn`
    or `view`), since that disturbs the user's game session;
  - running `install.ps1`, which refuses anyway while GTA is running.

  `host/lua.py` and `host/send.py` connect as a secondary client and are safe while GTA runs.
- Story mode only; never anything that touches GTA Online. BattlEye is off through the game's `args.txt`.
- Don't commit or copy game files or ScriptHookV/ReShade binaries; `third_party/` is fetched and git-ignored.

## Commands

The shell is Windows PowerShell 5.1 or Git Bash. Python is `.venv\Scripts\python.exe`, with numpy and PIL.

| Task | Command |
|---|---|
| Build the guest (MSYS2 clang64) | `tools\build-guest.ps1` (add `-Clone` the first time, to clone `..\vanadium` and apply `patches/`) |
| Run the guest | `tools\run-guest.ps1` (default place `places\Tools.rbxlx`; `-Place <rbxl>`, `-KeepMap`, `-Empty`) |
| Stop the guest | `.venv\Scripts\python.exe host\send.py '{"t":"quit"}'` (required before rebuilding: the exe is locked while it runs) |
| Fetch host dependencies | `host\gta\fetch_deps.ps1` |
| Build the host (MSVC) | `host\gta\build.ps1` builds `build\gta\GtrHost.asi`, `GtrCompositor.addon64` and `fakegta\` |
| Install into GTA | `host\gta\install.ps1` (GTA closed; `-Remove` uninstalls) |
| Run Luau in the guest | `.venv\Scripts\python.exe host\lua.py "return workspace.Gravity"` |
| Send a raw message | `host\send.py '<json>'`; `{"t":"host","op":"..."}` is relayed to GTA's script (e.g. `sunreload`, `wanted`, `debug`) |

Shader-only changes don't need a GTA restart. Copy `host/gta/shaders/GtrPassthrough.fx` into the game's
`reshade-shaders\Shaders\`; the add-on reloads the effect when the file changes. The sun table `GtrSun.txt` lives in
`%LOCALAPPDATA%\Gtr` and reloads with `op: sunreload`.

### Tests

There's no test runner: each `host\test_*.py` is a standalone script that prints `ok`/`FAIL` lines and exits non-zero on
failure. All of them need the guest running and GTA closed. Run them from `host\`, because they import each other:

```
cd host; ..\.venv\Scripts\python.exe test_weapons.py
```

- Most need a place (`tools\run-guest.ps1`), restarted fresh between suites:
  `test_bodies`, `test_weapons`, `test_trainer`, `test_drive`, `test_step`, `test_ground`, `test_pistol_range`.
- `test_transparency` and `test_compositor` need `tools\run-guest.ps1 -Empty`. A character in the scene makes
  `test_compositor` fail.
- `test_compositor` drives `build\gta\fakegta\fakegta.exe`: a D3D11 stand-in for GTA with the real ReShade add-on and effect.
- `host\fakehost.py` is the original end-to-end check of the frame export against ray-traced predictions.
- Restart the guest after testing so the user gets a clean session.

Logs:
- host script: `%LOCALAPPDATA%\Gtr\GtrHost.log`;
- guest: newest file in `build\vanadium\GtrGuest\logs` (FrameStats and Bridge costs every 10 s);
- ReShade: `<game>\ReShade.log` (effect compile errors).

## Architecture

```
GTA V Legacy process                                         gtr-guest.exe (Vanadium + guest/)
  GtrHost.asi (host/gta/src/script.cpp, ScriptHookV)  <-- TCP 127.0.0.1:25610, JSON lines -->  guest/Bridge.cpp
  GtrCompositor.addon64 (compositor.cpp) + GtrPassthrough.fx  <-- shared memory -->  guest/FrameExporter.cpp
```

**Spaces.** Everything on the wire is in host space: GTA metres, Z up. A host point `h` maps to the guest point
`(h.x-o.x, h.z-o.z, -(h.y-o.y)) * studsPerMetre`, where `o` is an origin the host sends once (`origin` message), at
0.35 m per stud. Guest (x, y, z) is therefore host (x, -z, y). Getting this sign wrong is the most common bug.

**Shared memory** (`shared/gtr_frame.h`, mirrored by `host/gtrframe.py`):
- `Local\GtrPassthroughFrame`: a ring of frame slots. Each holds:
  - colour plus depth in metres for a cropped rect around what the guest drew;
  - the GUI layer;
  - a light-view depth map for sun shadows;
  - the camera tick it was drawn for.
- `Local\GtrPassthroughHost` (`GtrHostState`, exactly 512 bytes): what the script tells the compositor each frame
  (camera ticks, sun direction/strength/tint, debug view and so on). New fields must come out of `Padding`, and the
  `static_assert` must still hold.

**Frame sync.** The script tags each camera it sends with a tick. The guest draws from that exact camera, and the
compositor picks the slot whose tick matches the frame GTA is presenting, so the character doesn't slide against the
world. In the guest, `TaskScheduler` job priorities are deliberate:
- `GtrReceive` 0: messages and input at the frame start;
- PreRender 1;
- `GtrDraw` 9994: export *before* physics, or the character slides;
- Present 9995, then Simulation 9996;
- `GtrBridge` 9999: report state;
- InputEndFrame 10000.

**Guest (`guest/`):**
- `Bridge` owns the link, origin, ground tiles, spawn pad, explosions, camera shake, seat state and the place scripts.
  It runs `guest/scripts/*.lua` (Dress, Trainer, Rcl, Pistol) into the place via `RunPlaceScript`.
  The character's position is always its Humanoid's root part (`GetRoot`/`PlaceCharacter`): a classic R6 model's
  PrimaryPart is its head.
- `places/Tools.rbxlx`, the default place, holds only the seven classic tools and an R6 StarterPlayer. It is written
  by `tools/make-tools-place.py` from a running place that has the tools, through the guest's `save` message (Vanadium's
  XML writer).
  `guest/Guest.cmake` copies `scripts/` and `models/` into the build. It is injected into Vanadium's CMake without
  editing it, through `cmake/GtrInject.cmake` (`CMAKE_PROJECT_Vanadium_INCLUDE`).
- `HostBodies` (`Bodies.cpp`): anchored proxy boxes for GTA's nearby peds, vehicles and objects, in the
  `workspace.HostBodies` folder. Each part carries a `HostId` StringValue.
  - Kinds: 0 object, 1 vehicle, 2 person, 3 rider.
  - Persons and riders are a Model holding a Humanoid and a "Torso" part, so classic tools hurt them. A rider is a
    non-colliding box at a car's side window.
  - Humanoid health changes go to the host as `hurt`; touches as `impulse`; vehicles overlapping guest parts as `crash`.
  - Building HopperBins moving a box become `move`, `copy` or `delete`, but only while a bin is active.
  - Fast loose parts are reported as `probes`; the host answers with `walls` slabs so they hit distant GTA geometry.
- `FrameExporter`: a second `SceneView` from GTA's camera. It packs colour, alpha and depth, using coverage for
  transparent parts and adorns. It also renders the GUI pass and the sun light view.

**Host script (`host/gta/src/script.cpp`):** one big file of free functions in an anonymous namespace.
`natives.h` holds hand-written native wrappers by hash; add new ones there. Each frame it:
- hides the player ped and carries it toward the character by velocity (`carry_player`);
- drives a scripted camera from the guest's camera;
- forwards input to the guest;
- samples ground with `GetGroundZFor3dCoord` around the character and sends tiles;
- sends nearby bodies and the sun;
- handles guest events: hurt, impulse, crash, explosion, move/copy/delete, seat/ride, probes→walls.

Host-side features include wanted levels, the HUD, doors, carjacking, the radio wheel and the trainer ops.

**Compositor:**
- `compositor.cpp` (ReShade add-on): uploads the matching slot's textures and sets the effect's uniforms from
  `GtrHostState`.
- `GtrPassthrough.fx` passes:
  - Light: relights the guest from GTA's picture;
  - Composite: depth test against GTA's reversed-Z depth, with fractional alpha;
  - Lamp: off by default;
  - Shadow: sun from the guest's light map with tent PCF, plus contact shadows;
  - Final: shadows tinted by the hour, then the GUI.

  The `DebugView` uniform switches debug outputs.

## Conventions

- Comments are full-sentence prose explaining *why*, in the voice already used throughout. Constants are named
  `kSomething` in the host and `sSomething` in the guest, with a comment giving units and the reason for the value.
- Measured values (sun table, shadow tint, shake curves, mark offset) come from in-game measurement tools:
  `host/sun_calibrate.py`, `calibrate.py`, `shake_record.py`, `prop_survey.py`. Their sources are noted beside each
  value. Keep that provenance when changing them.
