# gtr

A vibe-slopped Roblox-in-GTA passthrough mod that Claude Opus 5.5 did because I was bored

Note that this mod does not actually use the Roblox engine as its guest. It instead uses a completely custom engine made from the ground up that accurately loads and simulates .rbxl maps. That engine is also vibe-coded and it's for a different project. It shouldn't matter though because it accurately mimics the Roblox engine well enough in a way where you wouldn't be able to tell.

You probably won't be able to get this project working. Don't ask me for support, I didn't "make" it. The pipeline is convoluted and probably only works on my machine because of how hyper-specific it is to the setup I have.

Also this mod has no warranty and is made for entertainment purposes only.

# What Claude wrote

This mod puts a Roblox character into the world of Grand Theft Auto V. The character uses the lights and shadows of GTA V.

Two programs operate at the same time on one computer:

- The guest is Vanadium. Vanadium is an engine that is compatible with Roblox. It operates a Roblox place. The place has the character, the tools, the scripts and the interface.
- The host is GTA V Legacy in story mode. It supplies the world, the camera, the people and the vehicles.

The host shows the character, the bricks and the Roblox interface in its picture. The Roblox tools have an effect on the people, the vehicles and the objects of GTA V.

The two programs connect on this computer only. They do not use the internet.

> **WARNING:** Use this mod in story mode only. Do not start GTA Online with this mod installed. BattlEye is off when the mod is installed.

## Contents

- [Requirements](#requirements)
- [Install the mod](#install-the-mod)
- [Start the mod](#start-the-mod)
- [Controls](#controls)
- [Stop the mod](#stop-the-mod)
- [Remove the mod](#remove-the-mod)
- [Logs](#logs)
- [Troubleshooting](#troubleshooting)
- [How the mod operates](#how-the-mod-operates)
- [Build the mod](#build-the-mod)
- [Test the mod](#test-the-mod)

## Requirements

You must have these items:

- Windows 10 or Windows 11.
- GTA V Legacy from Steam (`GTA5.exe`). The mod does not operate with GTA V Enhanced.
- Optional: a copy of Vanadium in the folder next to this folder (`..\vanadium`). The first guest build copies the dependencies that it already downloaded.

You do not need a Roblox place file. The default place is `places\Tools.rbxlx` in this folder. It has only the classic Roblox tools.

To build the mod, you must also have these items:

- MSYS2 with the clang64 tools in `C:\msys64\clang64`. CMake and Ninja must be in that environment.
- Visual Studio with the C++ desktop tools (MSVC x64).
- Python 3 in `.venv`, with the `numpy` and `pillow` packages.
- Git.

## Install the mod

Do these steps one time only.

1. Close GTA V.
2. Open PowerShell in this folder.
3. Get the files for the host:

   ```powershell
   .\host\gta\fetch_deps.ps1
   ```

   This step gets ScriptHookV and ReShade from the internet. These files are not part of this repository.

4. Build the host:

   ```powershell
   .\host\gta\build.ps1
   ```

5. Install the host into GTA V:

   ```powershell
   .\host\gta\install.ps1
   ```

   The script finds the GTA V folder. If the script cannot find it, use `-GtaDir "<folder>"`.

6. Build the guest:

   ```powershell
   .\tools\build-guest.ps1 -Clone
   ```

   The `-Clone` option gets the Vanadium submodule in the `vanadium` folder and applies the engine patch. The first build compiles all of the engine. This can take a long time.

## Start the mod

1. Start the guest:

   ```powershell
   .\tools\run-guest.ps1
   ```

   A small Vanadium window opens. It waits for GTA V. Do not close this window.

2. Start GTA V Legacy from Steam.
3. Go into story mode.

The host connects to the guest automatically. Then the Roblox character replaces the GTA V player.

You can start the two programs in a different sequence. The host continues to try to connect until the guest is available.

To use a different place, give its file:

```powershell
.\tools\run-guest.ps1 -Place "C:\path\to\place.rbxl"
```

The guest removes the map of the place and keeps its tools and scripts. To keep the map, add `-KeepMap`.

The guest adds these items to each place: the building bins, the RCL, the pistol and the trainer menu.

To make `places\Tools.rbxlx` again, use a place that has the classic tools, for example Classic Crossroads:

```powershell
.\tools\run-guest.ps1 -Place "C:\path\to\Classic-Crossroads.rbxl"
.\.venv\Scripts\python.exe tools\make-tools-place.py
```

## Controls

The keys of the Roblox place operate the character. Examples are W, A, S, D, the space bar and the mouse.

The host adds these keys:

| Key | Function |
|---|---|
| F7 | Turn the mod on or off. When it is off, you have the GTA V player and camera again. |
| F | Get into the nearest GTA V vehicle. Push F again to get out. |
| Hold Q in a vehicle | Show the radio wheel. |
| E | Interact with GTA V items, as the GTA V player does. |
| Tilde (`) | Show or hide the backpack. |
| F8 | Put a brick in front of the character. |
| F9 | Show or hide the ground tiles. The tiles are usually invisible. |
| F10 | Move the character back to its start position. |
| F11 | Save a picture of the screen. |
| Page Up | Make the camera turn faster. |
| Page Down | Make the camera turn slower. |
| F6 | Change the frame offset. Do not change it. The correct value is 0. |

The backpack also has these items:

- The Roblox tools of the place.
- A trainer menu. Use it to add zombies and to change your wanted level.
- A rocket launcher (RCL).
- A pistol from 2009.
- The Clone, Grab, Move and Delete bins. These bins also operate on GTA V people, vehicles and objects.

## Stop the mod

1. Close GTA V.
2. Close the Vanadium window.

## Remove the mod

1. Close GTA V.
2. Remove the files from the GTA V folder:

   ```powershell
   .\host\gta\install.ps1 -Remove
   ```

The script keeps the logs that the game wrote in its folder.

## Logs

| Program | Location |
|---|---|
| Host script | `%LOCALAPPDATA%\Gtr\GtrHost.log` |
| Pictures from F11 | `%LOCALAPPDATA%\Gtr` |
| Guest | The newest file in `build\vanadium\GtrGuest\logs` |
| ReShade and the shader | `ReShade.log` in the GTA V folder |

## Troubleshooting

| Problem | Possible cause | Action |
|---|---|---|
| The character does not show in GTA V. | The guest is not available. | Make sure that the Vanadium window is open. If not, start the guest again. |
| The character does not show in GTA V. | The mod is off. | Push F7. |
| ScriptHookV shows "Unknown game version". | A GTA V update changed the game version. | Wait for a new version of ScriptHookV. Then do steps 3 and 5 of [Install the mod](#install-the-mod) again. |
| ScriptHookV shows an exception in `GtrHost.asi`. | An error in the host script. | Push OK. Send `GtrHost.log` to the developer. Write down what you did before the error. |
| `install.ps1` stops with "GTA V is running". | GTA V is open. | Close GTA V. Do the step again. |
| `build-guest.ps1` stops with "Permission denied". | The guest is open. | Close the Vanadium window. Do the step again. |
| The character falls through the ground. | GTA V did not load the ground at that position. | Wait one second. The host moves the character back onto the ground. If not, push F10. |

## How the mod operates

The guest and the host send data to each other on each frame.

- **From the host to the guest:**
  - The camera of GTA V.
  - The ground near the character, as invisible tiles.
  - A box for each person, vehicle and object near the character.
  - The position of the sun and the time of day.
  - The keyboard and the mouse.
- **From the guest to the host:**
  - The position of the character and the camera.
  - Damage to the GTA V people.
  - Hits on the vehicles and the objects.
  - Explosions.
  - The tool that the character holds.

The host has three parts:

- **GtrHost.asi** is a ScriptHookV script.
  - It hides the GTA V player and moves it with the character. Thus the GTA V world loads around the character, and the people and the police react to it.
  - It controls the GTA V camera.
  - It changes the Roblox events into GTA V events.
- **GtrCompositor.addon64** is a ReShade add-on. It gets each picture from the guest.
- **GtrPassthrough.fx** is a ReShade shader. It puts the picture of the guest into the GTA V picture. It uses the depth of each pixel. Thus GTA V buildings can hide the character. The shader also adds the light of GTA V to the character, and it adds the shadow of the character to the world.

The pictures move through shared memory. The messages move through a TCP connection on `127.0.0.1`, port 25610.

The file `MODDING_PLAN.md` gives the full design and the history of the decisions.

## Build the mod

Build the guest:

```powershell
.\tools\build-guest.ps1
```

The guest file is `build\vanadium\GtrGuest\gtr-guest.exe`.

> **CAUTION:** Close the Vanadium window before you build the guest. Windows locks the guest file while the guest operates.

Build and install the host:

```powershell
.\host\gta\build.ps1
.\host\gta\install.ps1
```

> **CAUTION:** Close GTA V before you install the host.

The guest uses a private copy of Vanadium in the `vanadium` folder. Do not change the Vanadium folder next to this folder. Put changes to the engine in `patches\vanadium-host-view.patch`.

You can change the shader while GTA V operates. Copy `host\gta\shaders\GtrPassthrough.fx` into `reshade-shaders\Shaders` in the GTA V folder. ReShade loads the shader again automatically.

## Test the mod

The tests do not use GTA V. They operate the guest as a host does.

> **WARNING:** Close GTA V before you start a test. A test sends data to the guest, and that data changes your game.

1. Start the guest:

   ```powershell
   .\tools\run-guest.ps1
   ```

   For `test_transparency.py` and `test_compositor.py`, add `-Empty`.

2. Go into the `host` folder:

   ```powershell
   cd host
   ```

3. Start the test:

   ```powershell
   ..\.venv\Scripts\python.exe test_weapons.py
   ```

Each test shows `ok` or `FAIL` for each check. At the end, it shows `PASS` or `FAIL`.

Start the guest again before a different test. A test can change the place.

| Test | What it examines |
|---|---|
| `test_bodies.py` | The boxes for the GTA V people, vehicles and objects |
| `test_weapons.py` | Damage from the weapons, also to people in vehicles |
| `test_pistol_range.py` | Hits from the pistol at different distances |
| `test_trainer.py` | The trainer menu and the zombies |
| `test_drive.py` | The keyboard, the mouse and the tools |
| `test_step.py` | The character and the camera are in the same frame |
| `test_ground.py` | The character stands and walks on the ground tiles |
| `test_transparency.py` | Parts that you can see through (use `-Empty`) |
| `test_compositor.py` | The add-on and the shader in a test window that operates as GTA V (use `-Empty`) |
