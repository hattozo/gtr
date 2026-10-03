# Vanadium inside GTA V: modding plan

A passthrough mashup. Vanadium (the guest) and GTA V Enhanced (the host) run side by side. Vanadium's character, bricks
and brickbattle tools are drawn into GTA's frame, hidden by GTA's buildings and lit like GTA's world, and what the
tools do happens in GTA too.

## What was found (1 Oct 2026)

| | |
|---|---|
| Guest | Vanadium at `492beca`: Roblox-compatible engine, wgpu-native 29, Luau, MSYS2 clang64. Its renderer already keeps a linear view-depth target per scene view. |
| Host | GTA V Enhanced, Steam, exe 1.0.1158.16, DX12, story mode. BattlEye already off (`commandline.txt` has `-nobattleye`). No loaders installed. |
| Script hook | **Blocked.** ScriptHookV's newest release (v3889.0/1158.13) lists its Enhanced builds as 1.0.1013.17/29/33/34 and 1.0.1158.13 (strings in the DLL). The install is 1.0.1158.16, released 17 Sep 2026, which it answers with "FATAL: Unknown game version". |
| Compositor | ReShade with add-on support. Its add-on API is the same on DX12, but the example this follows was DX11 (Legacy). Untested on Enhanced. |

## Design

```
GTA V Enhanced (story mode)                         gtr-guest.exe (Vanadium + guest/)
  GtrHost.asi (ScriptHookV)  -- TCP 127.0.0.1:25610, JSON lines -->  Bridge: camera, origin, bricks, player, ground, input
                             <-------------------------------------  events: explosions, hits, bricks placed
  ReShade add-on + effect    <-- shared memory "Local\GtrPassthroughFrame" --  FrameExporter: RGBA + depth in metres
    depth test against GTA's depth, relight, grade
```

- **Space.** Host space is GTA's (metres, Z up). A host point `h` is the guest point
  `(h.x - o.x, h.z - o.z, -(h.y - o.y)) / metresPerStud` for an origin `o` the host names once. 0.35 m per stud makes
  a classic character a pedestrian's height. Everything on the wire is in host space.
- **Frames.** The guest draws a second scene view from GTA's camera at GTA's resolution, packs colour (alpha = a
  part was drawn) and depth along the camera's forward axis in metres, and publishes them in a three-slot ring.
- **Lighting.** Vanadium is the renderer here, so "GTA's lighting" means driving Vanadium's `Lighting` from GTA
  (clock, sun direction, weather, ambient) and then matching GTA's grade in the compositor. Bricks can't be GTA
  props: GTA has no arbitrarily sized, coloured box, and the character and tools couldn't be props at all.
- **Vanadium is not edited.** Another session is changing ~355 files there. The guest lives in `guest/` and builds
  inside a private clone (`vanadium/`, a git submodule) through `cmake/GtrInject.cmake`. The clone carries one
  change, `patches/vanadium-host-view.patch`: a public accessor on `Gfx::Renderer`. After the big change
  lands, pull it into the clone and reapply that patch.

## Milestones and their oracles

| | Milestone | Oracle (what proves it) |
|---|---|---|
| M0 | Guest exports frames from a host camera | `host/fakehost.py`: bricks placed at known GTA coordinates must cover the pixels and depths its own ray tracer predicts (IoU >= 0.97, median depth error <= 1 cm), over six poses including a rolled camera and an odd resolution |
| M1 | A Vanadium brick in real GTA, hidden by GTA's world | A fake D3D12 host running the real add-on and effect first; then in game, a brick behind a lamppost and in front of a wall, screenshotted |
| M2 | The Vanadium character replaces GTA's player; GTA's ground is collision in Vanadium | Character's feet on the ground at five places (flat, slope, stairs, indoors, roof), screenshotted; the character's position error against GTA's player logged |
| M3 | Lighting follows GTA | Same brick at noon, dusk, night and in rain; its lit and shadowed faces compared with a GTA prop beside it |
| M4 | Brickbattle tools from `Classic-Crossroads.rbxl` work | Per tool, a scripted shot: rocket and bomb make GTA explosions, sword, slingshot, superball and paintball hit peds and cars, trowel walls stop GTA cars |
| M5 | Latency hidden; showcase | The gold-wall test from the Minecraft notes: guest-only geometry against a host-only skyline during a fast pan |

## Where it stands (1 Oct 2026)

- **M0 done.** `host/fakehost.py` passes all six poses: coverage IoU 0.98-0.99 (the rest is a one-pixel antialiasing rim),
  depth within 0.05 mm over 20-55 thousand pixels a pose.
- **M1, the half that needs no game, done.** `host/test_compositor.py` passes: the real add-on and effect, in ReShade 6.8.0
  inside a D3D11 stand-in with a reversed-Z depth buffer, show the guest where it is in front, hide it behind the host's
  pillar, and leave the rest of the host's picture untouched (each 100% of the pixels checked).
- **Not done or not verified:** anything in GTA itself; the add-on on D3D12 (it moves its textures between states for
  D3D12, but only D3D11 has run it); the ScriptHookV script that will send GTA's camera; re-projection for latency.
- **Seen working ahead of schedule:** `Classic-Crossroads.rbxl` loads in the guest with all seven tools in the Backpack
  (PaintballGun, RocketLauncher, Slingshot, Superball, Sword, Timebomb, Trowel), and the character with a tool equipped
  exports through the pipeline (`host/snap.py`).

- **Since then (same day, game rolled back to 1.0.1158.13):**
  - The guest loads a place without its map (`--keep-map` keeps it), gives the character a spawn pad, and turns `ground`
    messages into invisible collision tiles. `host/test_ground.py` passes: 2304 tiles, seven tools, the character spawns
    at the host's point, stands on the ground and walks up a 14-degree slope of 0.5 m tiles.
  - `GtrHost.asi` is written and builds: camera every frame, ground probes around the player (0.5 m cells out to 12 m,
    160 probes a frame), hotkeys, a log and pictures in `%LOCALAPPDATA%\Gtr`. Its link and camera maths are the ones the
    stand-in host now runs, and both earlier tests still pass with them. **It has not run in GTA yet.**
  - `host/gta/install.ps1` has put everything in the game folder, ReShade as `dxgi.dll`. Whether GTA Enhanced loads that
    `dxgi.dll` and ScriptHookV's `dinput8.dll` is the first thing the first run will show.

- **First run on Enhanced (1 Oct, 23:38), then the switch to Legacy.** The Enhanced install was damaged by the rollback
  and the owner is installing Legacy instead. What that one run showed, from `ReShade.log`:
  - ReShade loads as `dxgi.dll` on D3D12, loads the add-on, compiles the effect, connects to the guest's frames and
    finds a depth buffer. No frame was uploaded, since no camera was sent, so the D3D12 texture path is still unproven.
  - ScriptHookV did not load: no `asiloader.log`, and the game loaded `dinput8.dll` from `C:\WINDOWS\system32` by full
    path, past the ASI loader beside it. Whether that is Enhanced or the damaged install is unknown.
  - On Legacy every piece of this route is the one the reference example ran: D3D11, ScriptHookV through `dinput8.dll`,
    ReShade through the ASI loader (`install.ps1` picks that for Legacy and writes `args.txt` with BattlEye off).

- **First run on Legacy (2 Oct, 08:13): it works in the game.** Legacy 1.0.3889.0, D3D11, 1600x900. The ASI loader,
  ScriptHookV, ReShade as `ReShade64.asi` and the add-on all load; the effect compiles; ReShade finds the depth buffer; the
  guest exports about 95 frames a second. Screenshots in `out/game/`: the character stands on the floor beside Franklin
  at his size, and the ground tiles (F9) lie on GTA's floor, step up the stairs and rise at walls. Occlusion by GTA's
  world has not been looked at properly yet.
  - Found: the guest took one client only, so a tool connecting kicked the script off and the session started over
    (bricks cleared). Found: the guest drew over the pause menu, because GTA stops scripts there. Found: a restarted guest
    refused to start, because the game keeps the old frame mapping alive. All three are fixed below.
- **Second batch (2 Oct), built and tested without the game, not yet run in it:**
  - The owner asked for Roblox's camera in place of GTA's, with GTA's camera shakes still showing. So the script now
    passes the raw keyboard and mouse to the guest (`key`, `mouse`, `button`, `wheel`), where the place's own control,
    camera and tool scripts use them; the guest reports its character and camera (`state`); the script puts a scripted
    GTA camera there and carries GTA's hidden, frozen player along. The guest's picture is still drawn from the camera
    GTA finally rendered with, so whatever shakes GTA's view shakes the guest's the same. Unverified: that GTA applies
    its own explosion shakes to a scripted camera.
  - `host/test_drive.py` passes: W walks relative to the place's camera, Space jumps, a right-button drag turns the
    camera, the wheel zooms, a number key equips the rocket launcher and a click fires it; held keys are released when
    the host goes quiet; a second client works beside the host.
  - Guest explosions are reported (`explosion`) and the script sets off a GTA explosion there. GTA's clock drives the
    guest's (`light`), applied each frame over the place's own day and night script.
  - The compositor hides the guest when the script's heartbeat stops (pause menu). The script's log can be read while
    the game runs. `tools/screenshot-game.ps1` takes a picture through the compositor.
  - To tune in the game: mouse speed while the camera turns (`g_lookScale`, op `lookscale`), which way the wheel zooms.

- **Second run on Legacy (2 Oct): the owner played it.** Controls, camera and tools work; a rocket makes a GTA explosion
  where it lands. Reported: the character lags the camera and looks pasted on; no camera shake (GTA does not shake a
  scripted camera). Asked for: GTA's real shadow under the character.
- **Third batch (2 Oct), tested without the game, installed, not yet run in it:**
  - Lag: the guest draws from its place's own camera and each frame carries that pose (`view` replaces `cam` for such a
    host). The script puts GTA's camera at the pose of the newest frame and names that frame for the compositor
    (`PresentCameraId`), so both pictures are of one camera. The ring is 8 slots so the frame is still there. How many
    ticks GTA takes from camera to finished picture is not known: `g_poseLag`, default 1, F6 steps it.
  - Shake: an explosion shakes the camera the frames are drawn from (`Bridge::GetShake`), so GTA's follows.
  - Lighting: the effect relights the guest from the blurred host picture, matches its grade, softens edges and adds
    contact shadows; the guest itself is lit as a fixed early afternoon. Seen working in the stand-in only.
  - Shadow: the player is faded with SET_ENTITY_ALPHA 0 instead of made invisible, in the hope GTA still casts its
    shadow. Unverified.
  - `host/test_drive.py` now also checks the frame poses and the shake.

- **Third run (2 Oct): still sliding at every lag setting; no shadow; captures had the wrong colours.**
  - GTA ran at about 150 frames a second and the guest at about 170, so a tick is under 7 ms, and GTA draws on another
    thread: "the frame named a tick ago" is sometimes the picture being finished and sometimes not. A fixed lag can't work.
  - SET_ENTITY_ALPHA 0 removes the player's shadow too (checked at noon with a screenshot). GTA has no native for the
    sun's direction. A shadow cast in the compositor from the guest's depth needs that direction: not built.
  - GTA Legacy's back buffer is B8G8R8A8 and ReShade's capture_screenshot returned it in that order; the add-on now asks
    the format. Colours in every earlier picture in `out/game/` are red-blue swapped.
  - While the game was paused the guest dropped the script's connection (unread `state` lines filled its buffer) and the
    session started over on resume. `state` is now skipped for a client with a backlog.
- **Fourth batch (2 Oct), tested without the game, installed, not yet run in it:**
  - Each tick the script draws a mark into the picture (two 8-pixel squares, top left: the tick's number in the colour
    bits, then its opposite) and files the chosen frame's CameraId under that number (`GtrHostState::TickCamera`). The
    add-on keeps the frames of the last eight ticks in eight texture pairs; the effect reads the mark off the picture
    and samples that pair, then paints the mark over. F6 steps `MarkOffset` (0, 1, -1) in case GTA draws rectangles a
    tick apart from the camera.
  - `host/test_compositor.py` now swings the stand-in's camera at 100 degrees a second with a magenta box inside each
    brick: 1226 magenta pixels show taking the newest frame, 0 with marked pictures.
  - Unverified in the game: that script DRAW_RECTs land in the same picture as the camera set that tick, and that they
    are still drawn with the HUD hidden.
- **Fourth batch in the game (2 Oct):** the character still slid over GTA's ground, and had a pale rim.
- **Fifth batch (2 Oct). Guest half running in the game; host half built, not installed, not yet tested:**
  - The slide was the guest's, not the sync's. The export drew at the end of a frame (job 9999), after the physics step
    (9996), from the camera RenderStepped placed at its start (job 1): every view syncs its own part cache as it draws,
    so each frame showed the character one step ahead of its camera. Measured in the running game with
    `host/test_step.py --live` (hold D, then A; where the head is in the frames against where it rests): +11.7 and
    -12.1 cm, a 50 fps step at walking speed. The picture is now drawn by a job of its own at 9994, just before the
    renderer's (`Bridge::Draw`); hearing and answering the host stays at 9999. After: +0.6 and -0.6 cm. The marks could
    never have fixed this, and the stand-in's test, with nothing moving in the guest, could not see it.
  - The rim: the scene is antialiased, so outline pixels are part sky, and Vanadium's afternoon sky is bright. The pack
    shader now gives an outline pixel the colour of the wholly covered pixels beside it. Checked in `out/game/run5.png`.
  - Exports went from about 50 to about 87 frames a second with the same change (frames are published a job earlier).
  - Still unmeasured in the game: whether a frame is shown with the GTA picture of the same camera while the camera turns
    (the marks). For that, built but needing a game restart to install: script op `probe` (a magenta `DRAW_BOX` 2 cm
    inside a guest brick), op `markoffset`, `GtrHostState::ShowMark` and `DebugView` for tools, and
    `host/calibrate.py`, which swings the camera and counts magenta for each offset. The add-on also recompiles the
    effect when its file changes, so effect changes no longer need a restart. `brick` takes `"remove":true`.
  - ReShade does not notice a changed effect file by itself (tried in the running game).
  - The user confirmed in the game that the sliding is gone. With the game closed, all of `fakehost.py`,
    `test_compositor.py` (997 magenta pixels unsynced, 0 marked), `test_ground.py`, `test_drive.py` and `test_step.py`
    pass, and the host half is installed. Next: `host/calibrate.py` in the relaunched game.
- **The marks, measured in the game (2 Oct):** `host/calibrate.py`, from first person in Franklin's house, with the camera
  turning: 8 to 18 magenta pixels in each of ten pictures at offset 0 (12 at rest, so edge noise), about 6000 in most
  pictures at +1 and at -1. The mark is read in every picture. So script DRAW_RECTs do land in the picture of the
  camera set that tick, HUD hidden or not, and offset 0 is right; F6 has no use left.
  The tool had to learn two things on the way: the character stands in front of the probe in third person and takes
  the box's colour through the relighting (so it zooms to first person and shows the guest as drawn), and indoors the
  probe can land inside a wall (so it turns the camera until the brick is in view).
- **Sixth batch (2 Oct), running in the game:** sun shadows, contact shadows, GTA's own camera shake.
  - *Sun shadows.* The effect walks a ray from each of GTA's surfaces towards GTA's sun through the guest's depth (pass
    `Shadow`, smoothed in `Final`); the script publishes the sun in the camera's space (`GtrHostState::SunView`,
    `SunShadow`, `TanHalfFov`), none indoors, less in bad weather (those factors are by eye). `test_compositor.py` checks
    it in the stand-in against traced shadows (mode `sun`).
  - GTA has no native for its sun, so `host/sun_calibrate.py` measures it: the player on the airport runway, seen from
    straight above, photographed with and without (alpha 0 takes the shadow away) at each half hour; the shadow's
    direction and length give the sun. Result in `host/gta/GtrSun.txt`: shadows from 6:30 to 20:30, sun never below
    about 20 degrees, 56 degrees at noon, up to 64% darker; nothing found at night (the moon's are too faint to read).
  - **GTA keeps its shadow maps while the camera is still**: after SET_CLOCK_TIME the shadows stayed where they were, for
    minutes, until the camera moved. The first measurement was wrong all day for it; the tool now moves the bench camera
    after each change of hour.
  - A player at alpha 1 or 16 has no shadow either, and at 64 is plainly visible: no GTA-drawn shadow for the character.
  - *Contact shadows* are now by distance in the world between GTA's surface and the guest's, not by screen distance, and
    share the smoothed shadow pass.
  - *Shake.* The user: the made-up shake was not GTA's, and GTA's are smooth. `host/shake_record.py` has the script
    SHAKE_CAM a bench camera and writes down what GTA does to it; the guest plays `GRENADE_EXPLOSION_SHAKE` back (a 5 Hz
    swing of 2 degrees, no movement), scaled by distance and half again for a rocket-sized blast. `ROCKET_EXPLOSION_SHAKE`
    doesn't exist; SMALL/MEDIUM/LARGE swing 10 to 18 degrees at full strength and aren't used. How strongly GTA itself
    applies the shake at a given distance is not measured: the falloff (nine blast radii) is a guess.
  - New script ops for tools: `bench` (passthrough off, player frozen at a place, a camera of the script's), `benchcam`,
    `pedalpha`, `shaketrace`, `sunreload`. The add-on recompiles a changed effect, which is how the shadows were tuned
    with the game running.
  - Not run since the last effect and guest changes (they need the game closed): the offline tests.
  - `test_step.py --live` reads 7 cm when the character walks down Franklin's stairs: the stairs, not the frames.
- **Props for GTA's own shadow maps, surveyed (2 Oct):** the user asked whether the character can be in GTA's real shadow
  maps. Fading the player out doesn't do it (above), so the candidate is props hidden inside the character's parts.
  `host/prop_survey.py` (script op `modeldims`) asked the game the size of 21,631 object names; 15,060 have one.
  Props can't be scaled. Inside a 0.35 x 0.70 x 0.35 m limb: `prop_cs_heist_bag_02` and a dozen duffel bags like it,
  0.64 x 0.32 x 0.31 (91% of each side). Inside the 0.70 x 0.70 x 0.35 torso: nothing box-like (wheels, a drone, a tray);
  two of the bags side by side would be 0.64 x 0.64 x 0.31. Inside a 0.35 m head: `ng_proc_paintcan02a`, `v_ind_ss_box03`
  and `prop_beach_volball01`, all 0.31 across (the head's mesh is nearer 0.42, so larger ones weren't looked for).
  Sizes only: what the props look like, and whether straps or handles make their boxes larger than their bodies, is
  not checked. Not built.
- **Seventh batch (2 Oct), tested without the game, installed, not yet run in it:** GTA's physics entities.
  The user dropped the hidden-prop shadow idea and asked for interactions with physics entities, and for NPCs to react
  to the character.
  - *GTA to guest.* Each tick the script lists the peds, vehicles and objects within 30 m (ScriptHookV's worldGetAll*,
    64 at most, people and vehicles first) and sends `bodies`: those that are new or moved (`set`: id, kind, centre,
    forward, up, size, health) and every 30 ticks all ids still there (`keep`). The guest (`guest/Bodies.cpp`) keeps an
    invisible anchored part for each, sized by the model's dimensions; a person is a 0.55 x 0.4 x 1.8 m box, upright
    unless ragdolled, in a Model with a Humanoid (no neck needed, no state machine) so the place's tools hurt it by
    their own scripts.
  - *Guest to GTA.* A Humanoid's health dropping is sent as `hurt` and applied with SET_ENTITY_HEALTH with GTA's player
    as the instigator. An unanchored part (not the character's) hitting a body at 2 m/s or more is sent as `impulse`
    (mass x the speed it came in with, the bounce undone by the face it hit) and applied as a change of velocity by an
    assumed mass (object 40 kg, vehicle 1500, person 80), ragdolling a person from 1.5 m/s.
  - *NPCs and the character.* GTA's hidden player is now solid (still frozen and carried) and no longer invincible: its
    health is held at 5000 and what it loses each tick is sent as `harm` and taken off the character's Humanoid, except
    for 400 ms after one of the guest's own explosions. Wanted levels stay off.
  - `host/test_bodies.py` checks the guest half. Unknown until run in the game: whether a solid frozen player flings
    vehicles the character stands on or touches; how well bounding boxes fit; the worldGetAll* cost; whether
    SET_ENTITY_HEALTH with an instigator makes people react.
  - `test_step.py` must run on a fresh guest: after `test_drive.py` (camera zoomed far out) it fails.
- **Seventh batch in the game (2 Oct):** the character died every frame: GTA doesn't give its player 5000 health, and the
  script reported the difference each tick. Now the script reads back what health GTA did give and counts from that,
  the player is proof against blasts, fire and collisions, and the guest ignores any one blow over 150. GTA's doors
  ignore forces, so a door-shaped object (a thin upright slab) the character walks into is swung open through
  SET_STATE_OF_CLOSEST_DOOR_OF_TYPE and shut again 3.5 s later; the swing's sign was wrong at first (now -1, op
  `doorsign`). The character walking against an object or person sends an impulse too (`HostBodies::PushByWalking`).
  Objects over 3.5 m get no box: one in Franklin's house was 6 m.
- **Eighth batch (2 Oct), tested without the game, installed, not yet run in it:** the Roblox interface and pointer.
  The exporter draws the renderer's GUI (`SceneResources::Gui`, last frame's uploaded batches) again into a target of
  its own over nothing and exports it as a third layer of the frame (`GuiWidth`, `GuiHeight`, `GuiBlueFirst`; frame
  version 3, slot stride now 12 bytes a pixel). The add-on keeps the newest frame's in one texture (`GTRGUI`) and the
  effect lays it over the finished picture, premultiplied. The guest's window is now the size of the host's picture,
  so the interface is pixel for pixel, and `VNSetRobloxCursorEnabled(true)` has the pointer drawn as part of it (the
  arrow, a tool's icon, UserInputService.MouseIcon). The script hides GTA's arrow with SET_MOUSE_CURSOR_VISIBLE(FALSE)
  while still following GTA's cursor; op `cursor` picks another way (1 arrow shown, 2 pointer from mouse movement)
  should that not hide it. Seen offline in `out/gui_over_grey.png`: top bar, chat, leaderboard, hotbar, arrow.
- **Eighth batch in the game (2 Oct):** the interface and pointer show (the user's screenshot). Two complaints:
  - "Shadows don't seem to work": the script's weather guess had cut the shadow to 15% (it read the previous weather
    as overcast while GTA drew strong shadows), and from straight above the 0.7 m caster thickness rejects a character
    1.75 m deep. Now: the strength is blended over GET_CURR_WEATHER_STATE's two weathers with gentler factors, and the
    thickness goes from 0.7 m (level camera) to 1.9 m (looking down) by `GtrHostState::UpView`. A debug view
    "Shadows" (5) shows the sun and contact masks.
  - "The lighting inherits the colours around it": the tint came from close by, so a white character on terracotta
    turned pink. The tint is now from a wider blur and a quarter as strong (LightTint 0.25, LightBlur 4.6, changed in
    the running game through the add-on's reload). And the guest is lit from GTA's sun: the script sends `light` with
    `sun` (from the sun table), and `Bridge::FitSun` searches the clock and GeographicLatitude that put Vanadium's sun
    there (within 2 degrees for the three directions tried); with no sun it is the plain afternoon as before.
  - Host half of these built, not installed.
- **Ninth batch (2 Oct), tested without the game, installed:** the user, now at the PC and not over remote desktop: the
  interface flickers when it changes, the character is sluggish and the compositor slow, the camera turns too slowly,
  no wanted levels, no shadows from lights other than the sun, NPCs can't be sword-fought, a noob avatar, HopperBins,
  NPCs should be pushed about.
  - *Speed.* The guest exported 33-50 frames a second (GTA 57): since the eighth batch its window was the host's size, so
    the scene was drawn twice at full size. `patches/vanadium-host-view.patch` now also adds
    `Renderer::SetMainSceneDrawn(false)`: the window draws the interface alone. 80-89 a second in the game. The window's
    view was also what told the place's camera its ViewportSize (it read 1x1 without it, which `test_drive.py` caught):
    `Bridge::Draw` sets it. On GTA's side the add-on's layers are now keyed by guest frame, not by tick, with the layer
    of each tick passed in `GtrTickLayerA/B`: a frame is uploaded once however many ticks choose it.
  - *Flicker.* The interface was drawn from the GPU buffers of the frame before with the batches of this one; the
    exporter calls `GuiRenderer::Upload` first.
  - *Police.* Wanted levels are on; one star is made two at once (at one the police arrest, and GTA's arrest takes its
    player away); the guest's explosions are ADD_OWNED_EXPLOSION; the character's death clears the level.
  - *Fighting.* Last session's log had five `hurt` in all, and one person with 800 health. Damage is now out of the
    person's own health, the last of it kills (GTA's people are dying, not dead, at 100), a blow of 15 or more staggers,
    and a person hurt and standing is given TASK_COMBAT_PED against the player.
  - *Pushing.* The character walking into a person sends `impulse` with `walk`, every 80 ms, and the script slides the
    person along at 3 m/s with SET_ENTITY_VELOCITY.
  - *Sensitivity.* The look scale is 48 (was 24); Page Up and Page Down change it by a fifth, kept in `settings.txt`.
  - *The place.* `Bridge::DressPlace` runs a script once there is a player: BodyColors and part colours of the noob, and
    HopperBins Move, Clone, Delete, Grab in StarterPack and the Backpack. The tests expect 11 tools.
  - Not yet: shadows from other lights. Plan: find the brightest lit place or lamp in the picture (a weighted centre
    over the mips of the host's picture), take it for a point light at its depth, and march the guest's shadow away
    from it when the sun's is weak; in the effect only, tuned through the add-on's reload.
- **Tenth batch (2 Oct), tested without the game, installed, not yet run in it:**
  - *Still sluggish* after the ninth batch. In the game the guest fell from 90-160 frames a second to 18-30 whenever the
    player was out in the world. `SetFrameStatsInterval` (from the bridge's first tick: the engine resets it after
    `Open`) and timings of the bridge's own steps (`Bridge costs` in the guest's log) showed it: `publish`, copying each
    frame out of the mapped GPU buffer into the shared memory, 9-11 ms a frame for 17 MB; messages, ground tiles and
    bodies cost nothing to speak of. Reusing dropped ground tiles (kept, harmless) made no difference.
    Fix: only the part of the picture with anything in it is copied. `Bridge::FindDrawn` bounds the boxes of everything
    drawable (parts but the ground's and the bodies', explosions) in the camera's picture; the exporter copies that
    rectangle of colour and depth into its place in the slot (`RectX..RectHeight`, frame version 4); the add-on uploads
    that box of each layer and tells the effect each layer's rectangle (`GtrLayerRect0..7`), outside which a layer is
    taken for empty. Offline, `publish` is 0.5-1.2 ms. The interface layer is still copied whole.
    The user's machine runs much else at once, which makes copies slower but is not the cause.
  - *Trainer.* Place scripts are now files (`guest/scripts/*.lua`, copied beside the guest, run through `RunCommand`;
    message `script` runs one again): `Dress.lua` (noob, HopperBins) and `Trainer.lua`: a menu (M, or its button under
    the leaderboard) that spawns zombies (clones of the character, green, arms out, run from the trainer: walk at the
    nearest of the player and the host's people, hit for 5 a second), clears them, heals, drops a brick, and asks the
    host for a time of day, weather, or to lose the police. A script asks the host by leaving a StringValue
    `HostRequest` under workspace.HostCamera, which the bridge passes on. No Roblox vehicles: the user said to leave
    VehicleSeats to the other Claude instance working on Vanadium. `host/test_trainer.py`.
  - *GTA's vehicles.* F by a vehicle (within 5 m): its driver, if any, is put out and flees, the door opens, and for
    0.55 s the script sends `seat` (the driver seat's bone, 0.42 m up) while the guest moves the character there in a
    hop (`Bridge::HoldSeat`: root anchored, Humanoid.Sit); then GTA's player is put in the seat, GTA's driving controls
    are enabled and the driving keys no longer forwarded, the ground is no longer sampled, and `seat` is sent each tick
    put ahead by the vehicle's velocity times the age of the frame being shown (the guest's steady clock is the
    performance counter, as the script's is). F again, or the vehicle gone: `seat` with `out`, the character stands
    beside the vehicle. Untested in the game: all of the GTA half.
  - Still to do: shadows from lights other than the sun (in the effect, to be tuned in the running game).
- **Tenth batch in the game (2 Oct):**
  - Still 20-30 frames a second from the guest. The cropped copy was not the whole of it: every job of the guest ran four
    to five times slower in the game than without it (ImGui 0.75 to 3.9 ms). Windows starves a background window beside
    a game that keeps every core busy. `Bridge::Open` now sets HIGH_PRIORITY_CLASS and switches power throttling off:
    89-103 frames a second in the game. `publish` is still 4 ms, the interface layer copied whole.
  - "I can't click Roblox GUIs": messages were handled at the frame's end (job 9999), and what was pressed is forgotten
    at the end of the frame (InputEndFrame, 10000), before the interface next looks for it. They are now handled at the
    frame's start (`Bridge::Receive`, job 0). A ScreenGui sits 36 px below its own coordinates (the top bar's inset).
  - `BrickColor.Random` isn't in Vanadium (the trainer's brick button).
  - Wanted levels and the police work (seen in a capture: a police car, the character hurt).
  - *Lamp shadows*, in the effect: pass `Lamp` writes, for each pixel that is bright against the picture as a whole
    (LampContrast times its mean, at least LampBrightness), its weight and its weight times where its light would be in
    the camera's space: the pixel's own place lifted 0.3 m, or 4.5 m if it faces up (ground under a street lamp); the
    1x1 mip is the sums, and their quotient the light. `Shadow` marches towards it as it does towards the sun, less the
    farther the surface, the fainter the light and the stronger the sun's shadow. Seen once in the game: a police car's
    headlights found, the character's shadow cast away from them. It is a guess at one light from the picture, not
    GTA's lights. The stand-in's preset switches it off.
  - "GTA vehicles are very choppy": the camera followed the guest's frames, which trail the vehicle by the guest's delay.
    Built, not installed: while riding, the script ties GTA's camera to the seat as it is this tick, offset by how the
    place's camera stands to the character in the guest's last `state`, and shows the newest frame unmatched; the seat
    is no longer sent ahead of the vehicle.
- **Eleventh batch (2 Oct), tested without the game, installed, not yet run in it:**
  - The riding camera (above) and the lamp shadows' effect are installed.
  - *Vehicles running into the guest's things.* The user: a car that hits bricks should register the hit, where the
    physics only moved the bricks out of the way. A vehicle's entry in `bodies` now carries its velocity (18 numbers).
    `HostBodies::CrashVehicles`, each tick, for each vehicle going 1 m/s or more: the parts overlapping its box
    (`Workspace::GetPartBoundsInBox`; not the ground, the bodies or the character), the box's face each is at, and the
    speed the two close at through it. A loose part is knocked on and the vehicle slowed by their masses (the part's
    mass times 6 kg, the vehicle 1500 kg, bounce 0.15); against an anchored part the vehicle takes it all. The host is
    sent `crash` (id, dv, at, hard) and applies dv with SET_ENTITY_VELOCITY, and SET_VEHICLE_DAMAGE where it struck from
    3 m/s. `test_bodies.py`: 10 m/s into an anchored brick, -11.5 m/s; into a loose 4-stud cube, -1.7 m/s and the cube
    2.2 m on in half a second. Unknown until run in the game: how SET_ENTITY_VELOCITY sits with GTA's own vehicle
    physics from tick to tick.
- **Eleventh batch in the game (2 Oct):** "shadows look off (too big and they change wildly)". Both were the shadows
  worked out from the camera's picture: the guest taken for solid along the view (a wide band from above, with the arms
  out), and the lamp guessed from the brightest thing on screen.
- **Twelfth batch (2 Oct), tested without the game, installed, not yet run in it:** a shadow map for the sun.
  - With the host's sun known, the bridge keeps a camera `HostLightCamera` 700 studs towards it, looking at the character
    (or, without one, at what the frame's camera looks at), with a field of view that takes in 36 studs round it; the
    exporter draws a second view from it, packs its depth, and exports it with the frame: 512 x 512 float32 after the
    interface, with the light camera's pose and tangent in the slot (now 256 bytes; frame version 5).
  - The add-on keeps a map for each layer and works out, from the frame's camera and the light's, the three rows that
    turn a place in the frame's camera's space into a place in the map (`GtrSunRowA/B/C0..7`, `GtrSunTan0..7`). The
    effect's `sun_from_map` looks nine texels up (bias 0.09 m); the march through the picture remains for a guest that
    sends no map. Map and layer are of the same frame, so the shadow holds still as the camera turns.
  - The stand-in's sun mode now has the guest send maps: the traced-shadow check went from 90.8% to 98.9% of the
    shadowed ground. The map of the character alone is some 500 texels at 4.9 cm each (`out/sunmap.png`).
  - Lamp shadows are off (LampShadows 0) until a lamp can be placed steadily enough to draw a map from.
- **Thirteenth batch (2 Oct), the guest half running in the game, the host half built and not installed:** a long list
  from the user, while playing.
  - *Pixelated shadow, crawling when moving.* The light camera now moves a whole texel at a time (its place along its
    right and up rounded to the texel), takes in 22 studs (3 cm a texel), and the effect blends a tent of 4 x 4
    comparisons as a texture is filtered.
  - *The building tools on GTA's things.* Bodies' boxes are no longer locked, and carry a StringValue `HostId`.
    `HostBodies::FollowTools`: a box not where the host last put it has been dragged (`move`: the script puts the thing
    there, frozen until 400 ms after the last move); a box gone was hammered (`delete`: SET_ENTITY_AS_MISSION_ENTITY,
    DELETE_ENTITY; not made again for 3 s); a part in the workspace with a `HostId` is the clone tool's copy, and once
    it has rested 300 ms the host makes one like it there (`copy`: CREATE_VEHICLE, CLONE_PED, CREATE_OBJECT_NO_OFFSET)
    and the copy goes. With a HopperBin active, the body under the pointer is outlined (`DataModel::SetPartOutline`),
    and `FindDrawn` takes in whatever is outlined.
  - *Long-range projectiles hit nothing* (the guest's ground and bodies are only near the character). `probes`: the
    guest's unanchored parts going 6 m/s or more (16 at most) with where they are and how fast they go; the script
    traces each 0.4 s of flight (at least 3 m) through GTA's world with
    START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE (map, vehicles, people, objects) and answers `walls` with where it
    hits and the surface's normal; the guest puts a 10 x 10 x 2 stud invisible slab behind that surface for 1.2 s.
  - *Carjacking failed* (the character went back to where it stood): the driver told to leave was still in the seat, and
    GTA puts nobody in a taken seat. A driver still there when the player is to get in is put out at once
    (CLEAR_PED_TASKS_IMMEDIATELY) and the player gets in the tick after.
  - *Cars clip through bricks.* A loose part a vehicle runs into is set outside the vehicle's face at once, then given
    its share of the vehicle's speed; looked at four times as often.
  - *Pushing people teleports them* (SET_ENTITY_VELOCITY): now TASK_GO_STRAIGHT_TO_COORD 1.6 m the way the character
    walks, at most every 350 ms a person.
  - *E.* GTA's controls 38, 46, 51 are enabled on foot, and of GTA's HUD only the radar and components 1-9, 13, 14, 17,
    19-22 are hidden, so the help text that offers E shows.
  - *NPCs react to weapons.* `state` carries the held tool's name; the script gives GTA's player the matching weapon
    (rocket launcher RPG, RCL rifle, paintball pistol, sword machete, bombs sticky bombs, balls ball) and keeps the weapon
    object at alpha 0.
  - *Trainer:* wanted level +1 and -1 (op `wanted` with `by`). *Tilde* (VK_OEM_3) is forwarded as Roblox's Backquote.
  - *RCL* (`guest/scripts/Rcl.lua`, after BenBonez's RCL 2.01): a 1 x 1 x 2 Dark stone grey handle, studs on top,
    inlet below, a hinge on the face away from the holder; 30 shots at 0.13 s, 3 s reload (R, or empty), damage 10 to
    any Humanoid the ray meets first, the host's people's included; the beam two thin parts that neither collide nor are
    queried, 0.03 and 0.06 s; sounds rbxassetid://13775494 and 2691591. Its rays meet only the guest's things, not
    GTA's walls.
- **Fourteenth batch (2 Oct):**
  - *The 2009 pistol* ("undead coming", the user's model, kept as `guest/models/UndeadPistol.rbxmx` and copied into the
    guest's content as `rbxasset://gtr/UndeadPistol.rbxmx`): `guest/scripts/Pistol.lua` loads it with `GetObjects` (the
    command bar may) and mends two things: its bullet script hurt only a Humanoid whose model held a "creator" tag, so it
    never hurt anyone (now: whatever it hits, not its shooter), and its bullet mesh pointed at Roblox's 2009 install
    (`rbxasset://../../../shareddata/assets/...`, now the asset ids). Running in the game. `host/test_weapons.py`: the
    RCL and the pistol each hurt a host person they are fired at.
  - Built, not installed: the wanted stars show while the player is wanted (HUD component 1); Q held in a vehicle
    brings up GTA's radio wheel (control 85), the mouse then GTA's and the place's camera left alone; GTA's cursor is held
    where the place's pointer is while the place holds the pointer still (SET_CURSOR_POSITION), so it no longer jumps
    when the right button is let go.
  - *Transparency doesn't work:* the scene's view depth comes from the opaque pass alone, and the exporter takes a pixel
    with no depth for empty. A part with 0 < Transparency < 1 over the host's picture is dropped; over the guest's own
    opaque parts it blends with them. A right fix writes coverage and depth from the transparent pass too, a change to
    Vanadium's scene pipelines. Not done: waiting on the user, as the other instance is changing Vanadium.
  - Mistake: a host-role test (`test_weapons.py`) was run while the user had GTA open again; it sent the guest its own
    `origin`, moving the user's world. Restarting the guest had the script introduce itself again. Check that GTA5.exe
    isn't running before any test that sends `origin`, `ground`, `spawn` or `view`.
- **Fifteenth batch (2 Oct), tested without the game, installed:**
  - *Transparency*, as an engine patch (the user's choice, knowing the other instance has likely changed the same code):
    `PartPass::Coverage` and `fs_coverage` draw the see-through batches again into an RGBA16F target (nearest view depth
    kept by a Min blend in RGB, coverage laid over in A) against the resolved depth; the export view keeps it
    (`SetTransparentCoverage`) and draws no sky (`SetSkyDrawn(false)`), so where nothing opaque is its colour is the
    see-through parts' times their coverage. The pack shader gives such pixels that coverage as alpha and that depth, and
    the effect lays the guest over the host by its alpha. `host/test_transparency.py`: a brick at Transparency 0.5 comes
    out at alpha 0.50 at its own depth.
  - *The bins' outline on GTA's things* didn't show: an adorn is drawn over the finished picture with no depth, so the
    pack shader took its pixels for empty. The adorn batches now go into the coverage pass too. Dragging a GTA vehicle's
    box with Move was checked offline (selected with its number key; `Humanoid:EquipTool` doesn't activate a
    HopperBin): eight `move` messages, and its outline kept in the exported frame.
  - **Merge note:** `patches/vanadium-host-view.patch` will not apply to the other instance's Vanadium. When the clone is
    brought up to it, re-implement: GetSceneResources, SetMainSceneDrawn, and the coverage pass above.
- **Sixteenth batch (2 Oct), installed:** "the NPCs are stuck walking": a shoved person kept its
  TASK_GO_STRAIGHT_TO_COORD; CLEAR_PED_TASKS hands it back to GTA 700 ms after the last shove. (If people the character
  never touched get stuck too, suspect the solid, frozen, carried player.) "The character is in front of the radio
  wheel": GTA draws its HUD before the compositor, so the guest is hidden (`Active` 0) while the radio wheel is up.
- **Seventeenth batch (2 Oct), built:** people the character never touched were stuck walking on the spot near it: it
  was the frozen player, teleported each tick, an immovable post to them. GTA's player is now unfrozen, cannot ragdoll,
  and is moved towards the character by its velocity (the gap closed in 0.08 s; set outright when 2.5 m behind or 1.6 m
  off in height), so GTA's people push against it and step round it.
- **Repository (3 Oct):** first commit. The clone became the `vanadium/` submodule (GitHub's
  `noobwarrior-org/dieselnoob`), pinned at `492beca`, the commit before the checkout's "Added everything": that one
  changes the engine heavily and breaks the guest, so it waits until the owner asks for the merge. The patch stays
  applied but uncommitted in the submodule's tree. `build-guest.ps1 -Clone` now checks out the pinned commit instead of
  cloning `..\vanadium`, which it only reads for its downloaded `.cache`.

## In the game

The keyboard and mouse play the Roblox place: WASD and Space, right-button drag to turn the camera, wheel to zoom, number
keys for tools, click to use one. GTA's own controls are off but Esc (pause).

| key | |
|---|---|
| F6 | step the frame offset (0, 1, -1); 0 should be right, the others are for a guest that still slides when the camera turns |
| F7 | passthrough on or off; off gives GTA its camera, controls and player back |
| F8 | a brick in front of the character; again on the same spot stacks |
| F9 | show the guest's ground tiles, to check them against GTA's ground |
| F10 | bring the character back to where it started |
| F11 | save the finished picture to `%LOCALAPPDATA%\Gtr` (one is also saved 8 s after connecting) |

Where to look when it doesn't work: `%LOCALAPPDATA%\Gtr\GtrHost.log` (the script), and in the game folder `ReShade.log`
(add-on loaded? depth buffer found?), `asiloader.log` and `ScriptHookV.log`. The guest's log is the newest file in
`build\vanadium\GtrGuest\logs`. `host\gta\install.ps1 -Remove` takes it all out; `-ReShadeAs asi` loads ReShade through
the ASI loader instead.

## Running it

```powershell
tools\build-guest.ps1 -Clone            # once; later builds drop -Clone. First build compiles all of Vanadium
host\gta\fetch_deps.ps1                 # ScriptHookV SDK, ReShade headers and runtime into third_party\
host\gta\build.ps1                      # GtrCompositor.addon64 and the stand-in host, with MSVC

host\gta\install.ps1                    # into the game folder; -Remove undoes it

tools\run-guest.ps1                     # the guest with Classic Crossroads' tools and no map; -Place "" for an empty world
# or by hand (it needs MSYS2's clang64 DLLs on PATH):
#   build\vanadium\GtrGuest\gtr-guest.exe [place.rbxl] [--keep-map] [--metres-per-stud 0.35] [--port 25610]

.venv\Scripts\python host\fakehost.py           # M0's oracle (empty guest); pictures in out\fakehost
.venv\Scripts\python host\test_compositor.py    # M1's oracle (empty guest); picture in out\compositor
.venv\Scripts\python host\test_ground.py        # M2's guest half (guest with a place); pictures in out\ground
.venv\Scripts\python host\test_step.py         # the character is drawn at its camera's step (guest with a place); --live beside a running game
.venv\Scripts\python host\sun_calibrate.py      # in a running game: measure GTA's sun through its day (writes host\gta\GtrSun.txt)
.venv\Scripts\python host\shake_record.py       # in a running game: record GTA's camera shakes for the guest to play
.venv\Scripts\python host\calibrate.py          # in a running game: are frames shown with the pictures they belong to
.venv\Scripts\python host\snap.py out\me.png    # photograph the guest's character (guest playing a place)
.venv\Scripts\python host\lua.py "return workspace.Gravity"   # run Luau in the guest
```

Places with characters need Roblox's content beside the exe: copy `content`, `ExtraContent` and `PlatformContent` from a
Vanadium player build into `build\vanadium\GtrGuest`. The Python environment is `py -m venv .venv` plus numpy and Pillow.

Link messages (to the guest, one JSON object a line): `origin`, `cam`, `spawn`, `ground`, `brick`, `clear`, `light`,
`key`, `mouse`, `button`, `wheel`, `debug`, `where`, `lua`, `ping`, `quit`, and `host` (passed on to the script: ops
`time`, `weather`, `explode`, `lookscale`, `probe`, `markoffset`, `bench`, `benchcam`, `pedalpha`, `shaketrace`, `sunreload`, `modeldims`, `showbodies`). From the guest to the script: `state`, `explosion`, `hurt`, `impulse`, `crash`, and a place script's `HostRequest`. From the script also: `bodies`, `harm`, `shake`, `seat`, `script`. Several clients may be
connected; the one that sends the camera is the host.
The guest answers `hello` on connect and replies to `where`, `lua` and `ping`.

## Found along the way

- ReShade's depth detection ignores a frame that has one depth buffer and eight draw calls or fewer, so a stand-in host
  must draw in more calls than that or the effect gets no depth at all.
- A host that sends its last lines and closes at once lost them in the guest's link; fixed, and worth remembering for
  the script side.
- Crossroads looks dark through the pipeline because the place is at night (ClockTime 22.3) with its own colour
  correction. The export itself is display-referred and correct.
- The guest's shutdown leaves its export view alive on purpose: the engine closes its renderer before the app's own
  objects are destroyed.

## Open decisions

Both settled on 1 Oct 2026, following the owner's suggestion that Vanadium start empty and be fed GTA's collision:

1. **Who moves the character: Vanadium.** The character is simulated in Vanadium on collision fed from GTA, so jumping
   on trowel walls, rocket knockback and the sword lunge are Vanadium's own. GTA's player, hidden, is carried along so
   GTA's world streams and reacts around it, and GTA's camera keeps following it. Next to build: the script forwards
   movement keys as a direction relative to GTA's camera, and the guest reports the character's position back.
2. **Crossroads' map: left out.** Only what the place gives its players is kept (tools, scripts, GUIs).

Still open: what GTA's peds and cars collide with. Vanadium parts are drawn by the compositor, not as GTA objects, so
GTA can't feel them yet; invisible GTA props standing in for parts is the likely answer.

## Risks

- **ScriptHookV vs 1.0.1158.16.** It will refuse this build. The ways on, in order of effort: wait for its update;
  roll the game back to 1.0.1158.13 (Steam depot); or write our own native invoker for .16 by reverse engineering.
  The owner chooses. Everything up to M1's fake host is unaffected.
- **Upscalers and TAA on Enhanced.** With DLSS/FSR the depth buffer is at render resolution and jittered. First
  runs use native resolution with the upscaler off.
- **Transparent things over empty space.** Explosions, glass and particles don't write depth, so the exporter drops
  them where no part is behind them. Needs the scene's own alpha, a second small engine change, before M4's
  explosions look right.
- **Online.** Story mode only, BattlEye off. Nobody automates GTA's landing page while a person is at the
  keyboard.
