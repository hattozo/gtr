// GtrHost.asi: the ScriptHookV script that makes GTA V the host of the passthrough.
//
// GTA lends the guest its world, its window and its keyboard and mouse; the guest (Vanadium, playing a Roblox place) is the
// game being played. Every frame the script
//   - passes the keyboard and mouse on to the guest, where the place's own control, camera and tool scripts use them;
//   - puts GTA's camera where the place's camera is, and GTA's player, hidden, where the place's character is, so GTA's
//     world streams and reacts around it;
//   - sends back the camera GTA really rendered with (shakes and all), which the guest draws its picture from;
//   - probes the height of GTA's ground around the player and sends it as tiles the guest collides with;
//   - sets off in GTA the explosions the guest reports.
// It tells the compositor (GtrCompositor.addon64, a separate module in this process) the camera's clip planes and when to
// hide the guest, through GtrHostState.
//
//   F6   step the frame offset (0, 1, -1): 0 is right (measured, host/calibrate.py)
//   F7   passthrough on or off (off gives GTA its own camera, controls and player back)
//   F8   a brick in front of the character
//   F9   show or hide the guest's ground tiles, to check them against GTA's ground
//   F10  bring the guest's character back to where it started
//   F11  save the finished picture
//   Page Up, Page Down   the camera turns faster, slower
//   F    by one of GTA's vehicles, get in and drive it with GTA's own keys; F again to get out
//
// Story mode only. The log and the pictures go to %LOCALAPPDATA%\Gtr.
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <iterator>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <main.h>

#include "gta_math.h"
#include "gtr_frame.h"
#include "link.h"
#include "natives.h"

namespace
{
	// The ground is sampled on a grid of cells this large, out to this far from the player; the guest collides with a tile
	// for each. Cells are forgotten beyond the outer radius, which is larger so walking back and forth doesn't churn them.
	constexpr float kGroundCell = 0.5f;
	constexpr float kGroundRadius = 12.0f;
	constexpr float kGroundDropRadius = 16.0f;
	constexpr float kGroundDepth = 3.0f;
	// A probe starts this far above the player's origin (itself about a metre above the feet): low enough to find the floor
	// indoors instead of the roof
	constexpr float kGroundProbeAbove = 1.5f;
	constexpr int kGroundProbesPerTick = 160;
	// Known cells are probed again a few a tick, since GTA's collision streams in and changes
	constexpr int kGroundRechecksPerTick = 24;
	// The cells this near the character (metres) are all probed again every tick: the ground right under its feet is the
	// one that matters, and a cell first probed from below a bridge or from the foot of stairs has the wrong height
	constexpr float kGroundNearRadius = 2.0f;
	// The character is put back on GTA's ground when it is this far below it (metres) with nothing under its feet, having
	// fallen through where no tile was; not more often than this (milliseconds)
	constexpr float kRescueBelow = 1.0f;
	// The sun's shadow turns from day's grey to sunset's blue between these hours (see sun_tint)
	constexpr float kSunsetTintFrom = 17.0f;
	constexpr float kSunsetTintFull = 18.75f;
	constexpr float kRescueFrom = 60.0f;
	constexpr DWORD kRescueInterval = 1000;
	constexpr float kGroundChange = 0.03f;
	constexpr int kGroundDropInterval = 30;
	constexpr size_t kGroundTilesPerMessage = 400;

	// A classic 4 x 1.2 x 2 stud brick at 0.35 m a stud
	constexpr float kBrickSize[3] = {1.4f, 0.7f, 0.42f};
	constexpr float kBrickDistance = 2.5f;
	constexpr float kBrickColors[][3] = {
		{0.77f, 0.16f, 0.11f}, {0.05f, 0.41f, 0.67f}, {0.96f, 0.80f, 0.19f}, {0.29f, 0.59f, 0.29f}, {0.95f, 0.95f, 0.95f}};
	constexpr int kAutoCaptureMilliseconds = 8000;

	// GTA's player is moved to the character by a speed that closes the gap in this long (seconds), and put there outright
	// when it is this far behind across, or this far up or down (metres)
	constexpr float kCarrySeconds = 0.08f;
	constexpr float kCarryFarthest = 2.5f;
	constexpr float kCarryHighest = 1.6f;
	// GTA's player origin is about this far above its feet
	constexpr float kPedOriginHeight = 1.0f;
	// What the guest last said is acted on for this long; a guest that has gone quiet stops moving GTA's camera and player
	constexpr DWORD kStateMilliseconds = 500;
	constexpr float kGuestClock = 13.5f;
	// GTA's controls (the indices of its control enum)
	constexpr int kControlLookLeftRight = 1, kControlLookUpDown = 2;
	// Q in a vehicle, held: GTA's radio wheel
	constexpr int kControlRadioWheel = 85;
	constexpr int kControlWheelPrevious = 15, kControlWheelNext = 14;
	constexpr int kControlPause = 199, kControlPauseAlternate = 200;
	constexpr int kControlCursorX = 239, kControlCursorY = 240, kControlScrollUp = 241, kControlScrollDown = 242;
	// GTA's explosion types, and the size of blast from which the guest's count as the larger
	constexpr int kExplosionGrenade = 0, kExplosionRocket = 4;
	constexpr float kLargeBlastRadius = 3.0f;
	// The probe a tool asks for to check that the guest's frames and GTA's pictures are shown together (host/calibrate.py):
	// a box GTA draws, this much smaller all round than a brick the guest draws around it. Shown together, the brick hides
	// the box whatever the camera does; a frame apart, the box shows at the brick's edge while the camera turns.
	constexpr float kProbeSize = 1.0f;
	constexpr float kProbeInset = 0.02f;
	constexpr float kProbeDistance = 3.0f;
	// GTA's vehicles, people and loose objects near the character are told to the guest, which collides with a box for each.
	// So many at most, the nearest first, within this far; an object larger than this is part of the map, and one smaller
	// than that isn't worth a box
	constexpr float kBodyRadius = 30.0f;
	constexpr size_t kMaxBodies = 64;
	constexpr float kLargestObject = 3.5f;
	constexpr float kSmallestObject = 0.25f;
	// A person's box: GTA measures its people with their arms out
	constexpr float kPersonSize[3] = {0.55f, 0.4f, 1.8f};
	// A body is told again when it has moved this far, in metres or as a change in its axes
	constexpr float kBodyMoved = 0.01f;
	constexpr int kBodyKeepInterval = 30;
	// GTA's kinds of entity, and a person's health: dead at 100, whole at 200
	constexpr int kEntityPed = 1, kEntityVehicle = 2, kEntityObject = 3;
	constexpr int kPedDeadHealth = 100;
	// What the guest's things weigh against, when one of them pushes one of GTA's: kilograms, by kind
	constexpr float kBodyMass[3] = {40.0f, 1500.0f, 80.0f};
	constexpr float kLargestPush = 15.0f;
	// A person riding in a vehicle is a box from this far inside their seat to this far out past the vehicle's side (as far as
	// a fast shot goes in a step of the guest's physics, so it meets them before the vehicle), this
	// long and this high, its middle this far above their origin as they sit (metres)
	constexpr float kRiderInside = 0.25f;
	constexpr float kRiderOutside = 0.5f;
	constexpr float kRiderSize[2] = {1.2f, 1.1f};
	constexpr float kRiderRise = 0.35f;
	// A vehicle hit by something of the guest's is knocked this many times harder than its weight alone would have it: a ball
	// against a car's 1500 kg barely moved it, and the place's balls are meant to knock things about
	constexpr float kVehicleKnock = 6.0f;
	// A person knocked down fights back once up again, or after this long (milliseconds); told to at once, some got straight
	// up instead of falling
	constexpr DWORD kFightBackAfter = 400;
	// A person hit this hard (a change of speed, metres a second) falls over
	constexpr float kRagdollPush = 1.5f;
	// Page Up and Page Down make the camera turn faster and slower for the same movement of the mouse, by this much a press,
	// and the choice is kept in this file in the output folder
	constexpr float kLookScaleStep = 1.2f;
	constexpr float kLookScaleRange[2] = {4.0f, 400.0f};
	constexpr const char *kSettingsFile = "settings.txt";
	// A blow from one of the place's tools as hard as this, out of a person's whole health, staggers them
	constexpr float kStaggerDamage = 15.0f;
	// The character walking into a person has them step this far out of its way at this pace (GTA's: 1 walks, 2 runs)
	constexpr float kShoveSpeed = 2.0f;
	constexpr float kShoveStep = 1.6f;
	constexpr DWORD kShoveMilliseconds = 350;
	// The character rides GTA's vehicles: F by one gets in, F again gets out. Getting in takes this long, while the door
	// opens and the guest moves its character to the seat; the character sits this far above the seat's bone; and it gets
	// out this far to the vehicle's left
	constexpr float kRideReach = 5.0f;
	constexpr DWORD kRideEnterMilliseconds = 550;
	constexpr float kRideSeatHeight = 0.42f;
	// The guest tells where the character's feet are, which is this far under the middle of it that sits in the seat
	constexpr float kRideRootAboveFeet = 1.05f;
	constexpr float kRideExitSide = 1.7f;
	constexpr int kDriverSeat = -1, kDriverDoor = 0;
	constexpr int kLeaveVehicleAtOnce = 16;
	constexpr const char *kSeatBones[] = {"seat_dside_f", "seat_f"};
	// GTA's controls that drive, which are GTA's again while the character rides
	constexpr int kDrivingControls[] = {59, 60, 61, 62, 63, 64, 71, 72, 73, 74, 76, 86, 87, 88, 89, 90, 107, 108, 109, 110, 111, 112, 129, 130, 133, 134};
	// The weapon GTA's player holds for a tool the character holds, by words in the tool's name: GTA's people take fright
	// at an armed player, and run, or fight, or call the police. The weapon itself is kept out of sight
	struct ToolWeapon
	{
		const char *word;
		const char *weapon;
	};
	constexpr ToolWeapon kToolWeapons[] = {
		{"Rocket", "WEAPON_RPG"}, {"RCL", "WEAPON_CARBINERIFLE"}, {"[", "WEAPON_CARBINERIFLE"}, {"Paintball", "WEAPON_PISTOL"},
		{"Sword", "WEAPON_MACHETE"}, {"Timebomb", "WEAPON_STICKYBOMB"}, {"Bomb", "WEAPON_STICKYBOMB"}, {"Slingshot", "WEAPON_BALL"},
		{"Superball", "WEAPON_BALL"}};
	// GTA's own uses of E on foot: what a help text offers ("Press E to ..."), picking up, talking
	constexpr int kInteractControls[] = {38, 46, 51};
	// GTA's HUD is the place's while it is played, but for the help text that says what E would do there, its subtitles,
	// and the wanted stars: everything else of it is hidden (the radar, and these)
	constexpr int kHiddenHud[] = {2, 3, 4, 5, 6, 7, 8, 9, 13, 14, 17, 19, 20, 21, 22};
	// The wanted stars show while the player is wanted, which GTA itself decides
	constexpr int kHudWantedStars = 1;
	// The keys that drive, as Roblox numbers them: W A S D, Space, the arrows, Shift and Ctrl. They are not the guest's then
	constexpr int kDrivingKeys[] = {119, 97, 115, 100, 32, 273, 274, 275, 276, 304, 306};
	// A frame the script shows was drawn this much before; with the picture GTA is still to draw of it, this much more
	constexpr double kFrameAgeLimit = 0.4;
	constexpr double kShownAfterSeconds = 0.012;
	// A vehicle that runs into something of the guest's at this speed (metres a second) or more is dented, by so much a
	// metre a second and no more than this, over this radius
	constexpr float kCrashDentSpeed = 3.0f;
	constexpr float kCrashDentPerSpeed = 45.0f;
	constexpr float kCrashDentMost = 700.0f;
	constexpr float kCrashDentRadius = 0.6f;
	// A driver with something of the guest's in front of it (a "blocked" message, see the guest's LookAhead). It is slowed no
	// harder than this (m/s each second, a firm stop) to come to rest this far short of it (metres), and goes on as before
	// once the guest has said nothing for this long (ms)
	constexpr float kBlockedBraking = 7.0f;
	constexpr float kBlockedStopShort = 0.8f;
	constexpr DWORD kBlockedForget = 500;
	// Stopped, it waits a while, between these (ms), sounding the horn now and then, for between these each time and these
	// apart; then it loses patience, leans on the horn for this long, and pushes through at this speed (m/s, a slow roll, at
	// which the guest tips a trowel wall over instead of scattering it)
	constexpr DWORD kBlockedPatience[2] = {3000, 7000};
	constexpr DWORD kBlockedHorn[2] = {200, 900};
	constexpr DWORD kBlockedHornApart[2] = {900, 2600};
	constexpr DWORD kBlockedFedUpHorn = 1500;
	constexpr float kBlockedPushSpeed = 2.2f;
	// Below this (m/s) a vehicle counts as stopped
	constexpr float kBlockedStill = 0.5f;
	// A painted person or prop (see publish_paint) is coloured within its model's box made this much larger, from this high
	// above its lowest point (metres), as far as this from the camera (metres), the paint taking this much of its colour
	constexpr float kPaintMargin = 1.04f;
	constexpr float kPaintAboveGround = 0.02f;
	constexpr float kPaintFarthest = 120.0f;
	constexpr float kPaintStrength = 0.85f;
	// The guest's fast things are looked ahead of by this long a flight, and this far at the least; so many at a time
	constexpr int kMostProbes = 16;
	constexpr float kProbeSeconds = 0.4f;
	constexpr float kProbeLeast = 3.0f;
	// A door, by its shape (metres): a slab no thicker than this, so wide and so tall. It stays open this long after a push
	constexpr float kDoorThickest = 0.4f, kDoorNarrowest = 0.6f, kDoorWidest = 2.4f, kDoorLowest = 1.7f, kDoorTallest = 3.6f;
	constexpr DWORD kDoorOpenMilliseconds = 3500;
	// The player's health is asked to be this high, so that no one blow kills GTA's player; what it loses is the character's
	// to lose. GTA may give less
	constexpr int kPlayerHealth = 2000;
	// For this long after an explosion of the guest's, what the player loses is not passed on: the guest has done its own
	constexpr DWORD kOwnBlastMilliseconds = 400;

	// The bench a tool measures GTA itself on (host/sun_calibrate.py, host/shake_record.py): the passthrough is off, GTA's
	// player stands still on open ground, and a camera of the script's own looks at it from where the tool says
	constexpr float kBenchCameraOffset[3] = {0.0f, 0.0f, 30.0f};
	constexpr float kBenchCameraRotation[3] = {-90.0f, 0.0f, 0.0f};
	constexpr float kBenchCameraFov = 50.0f;
	constexpr float kBenchClearRadius = 100.0f;
	// Where the sun and moon are through GTA's day, as host/sun_calibrate.py measured it
	constexpr const char *kSunFile = "GtrSun.txt";
	// The guest's pointer modes, as Roblox numbers them
	constexpr int kPointerLockedToCentre = 1;

	// The keys passed on to the guest, as Windows names them and as Roblox does
	struct KeyMapping
	{
		int windows;
		int roblox;
	};
	constexpr KeyMapping kSpecialKeys[] = {
		{VK_SPACE, 32}, {VK_LSHIFT, 304}, {VK_RSHIFT, 303}, {VK_LCONTROL, 306}, {VK_TAB, 9}, {VK_BACK, 8}, {VK_RETURN, 13},
		{VK_UP, 273}, {VK_DOWN, 274}, {VK_RIGHT, 275}, {VK_LEFT, 276}, {VK_OEM_COMMA, 44}, {VK_OEM_PERIOD, 46},
		{VK_OEM_3, 96}};
	constexpr int kMouseButtons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};

	HMODULE g_module = nullptr;
	GuestLink g_link;
	GtrHostState *g_state = nullptr;
	// GTA's people and props the paintball gun has painted (vehicles are painted in GTA itself), by entity, with the colour,
	// and the boxes the compositor colours them in (GtrPaintState)
	std::unordered_map<int, std::array<float, 3>> g_painted;
	GtrPaintState *g_paint = nullptr;
	FILE *g_log = nullptr;
	std::string g_folder;

	bool g_enabled = true;
	bool g_groundShown = false;
	int g_generation = 0;
	int g_bricks = 0;
	int g_captures = 0;
	DWORD g_connectedAt = 0;
	bool g_autoCaptured = false;
	double g_origin[3] = {};
	// Where GTA's player stood when the guest connected: where the guest's character starts, and comes back to on F10
	double g_start[3] = {};
	// How many pixels of pointer movement one unit of GTA's look control stands for; a tool can change it while it runs
	float g_lookScale = 48.0f;
	// Added to the number the compositor reads off a picture's mark, in case GTA draws a tick's mark and uses its camera a
	// tick apart: stepped with F6, and 0 unless the guest slides against GTA's world when the camera turns
	int g_markOffset = 0;

	// What the guest last reported: its character (feet, heading, health) and its place's camera
	struct GuestState
	{
		bool hasCharacter = false;
		double character[5] = {};
		// The name of the tool the character holds, or nothing
		std::string tool;
		bool hasCamera = false;
		double camera[7] = {};
		int pointerLock = 0;
		DWORD at = 0;
	};
	GuestState g_guest;

	// While the guest is being played: GTA's camera is the scripted one, and its player is hidden and carried along
	bool g_playing = false;
	Cam g_camera = 0;
	float g_pointer[2] = {0.5f, 0.5f};

	struct Bench
	{
		bool on = false;
		Cam camera = 0;
		// Where the player stands, where it stood before, and whether GTA's ground there has been found yet
		float place[3] = {};
		float back[3] = {};
		float feet = 0.0f;
		bool grounded = false;
		// The camera, from the player's feet
		float offset[3] = {};
		float rotation[3] = {};
		float fov = kBenchCameraFov;
		int alpha = 255;
		// A camera shake of GTA's being written down, tick by tick
		FILE *trace = nullptr;
		DWORD traceStart = 0, traceUntil = 0;
	};
	Bench g_bench;

	// A survey a tool asks for (host/prop_survey.py): how large each of a list of GTA's models is, written to a file a few
	// models a tick. Loading each model first is slower, and is for the ones GTA won't measure unloaded.
	struct Survey
	{
		std::vector<std::string> names;
		size_t next = 0;
		FILE *out = nullptr;
		bool load = false;
		int waited = 0;
	};
	Survey g_survey;
	constexpr int kSurveyModelsPerTick = 300;
	constexpr int kSurveyLoadTicks = 120;

	// A row of the sun file: the hour of GTA's day, the direction towards the sun or moon (whichever casts the shadows
	// then), and how much darker its shadows are than its light
	struct SunRow
	{
		float hour;
		float direction[3];
		float shadow;
	};
	std::vector<SunRow> g_sun;

	// What the guest was last told of each body: its centre, forward, up and size, and its health
	struct BodySent
	{
		float numbers[12];
		int health;
	};
	std::unordered_map<int, BodySent> g_bodies;
	bool g_bodiesShown = false;
	DWORD g_ownBlastAt = 0;
	bool g_harmReady = false;
	// The radio wheel is up: the mouse chooses a station, and the place's camera is left where it is
	bool g_radioWheel = false;
	// The things a building tool of the place's has hold of, and until when: kept still meanwhile
	std::unordered_map<int, DWORD> g_held;
	// Vehicles whose drivers have the guest's things in front of them, by entity
	struct Blocked
	{
		float gap = 0.0f;
		DWORD heard = 0, lastTick = 0;
		// When it came to a stop (0 while it hasn't), when it loses patience, and when it next sounds its horn
		DWORD stoppedAt = 0, fedUpAt = 0, hornAt = 0;
		bool fedUp = false;
	};
	std::unordered_map<int, Blocked> g_blocked;
	std::minstd_rand g_random(GetTickCount());
	// The people the character has shoved aside, and when to hand them back to GTA
	std::unordered_map<int, DWORD> g_shoved;
	// GTA's people the character has struck, who are to fight it once they are on their feet: from when
	std::unordered_map<int, DWORD> g_fightBack;
	constexpr DWORD kHeldMilliseconds = 400;
	// The character's ride: on foot, getting in (the door open, the guest moving the character to the seat), or riding
	enum class Ride { OnFoot, GettingIn, Riding };
	struct RideState
	{
		Ride state = Ride::OnFoot;
		Vehicle vehicle = 0;
		DWORD since = 0;
	};
	RideState g_ride;
	// How long ago the frame now shown was drawn, smoothed: what the guest is told of a moving vehicle is put this far ahead
	double g_frameAge = 0.05;
	int g_sunSentTo = -1;
	// How the guest's pointer is found: GTA's own cursor with its arrow hidden, or shown, or the mouse's movement added up
	constexpr int kCursorHidden = 0, kCursorShown = 1, kCursorFromMovement = 2;
	int g_cursorMode = kCursorHidden;
	// The doors the character has pushed open, to be let shut again
	struct OpenDoor
	{
		int entity;
		Hash model;
		float position[3];
		DWORD until;
	};
	std::vector<OpenDoor> g_doors;
	// Which way a door swings for a push on its face; a tool can turn it round
	float g_doorSign = -1.0f;
	int g_playerHealth = 0;

	bool g_probe = false;
	float g_probeCentre[3] = {};

	std::unordered_map<uint64_t, float> g_ground;
	std::vector<std::pair<int, int>> g_groundOrder;
	size_t g_recheck = 0;

	void log_line(const char *format, ...)
	{
		if (g_log == nullptr)
			return;
		SYSTEMTIME now;
		GetLocalTime(&now);
		fprintf(g_log, "%02d:%02d:%02d.%03d ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
		va_list arguments;
		va_start(arguments, format);
		vfprintf(g_log, format, arguments);
		va_end(arguments);
		fputc('\n', g_log);
		fflush(g_log);
	}

	void sendf(const char *format, ...)
	{
		char line[1024];
		va_list arguments;
		va_start(arguments, format);
		vsnprintf(line, sizeof(line), format, arguments);
		va_end(arguments);
		g_link.send_line(line);
	}

	void open_outputs()
	{
		char local[MAX_PATH] = {};
		if (GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof(local)) == 0)
			return;
		g_folder = std::string(local) + "\\Gtr";
		CreateDirectoryA(g_folder.c_str(), nullptr);
		// Shared, so the log can be read while the game runs
		g_log = _fsopen((g_folder + "\\GtrHost.log").c_str(), "w", _SH_DENYNO);

		const HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(GtrHostState), GTR_HOST_MAPPING_NAME);
		g_state = mapping != nullptr ? static_cast<GtrHostState *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GtrHostState))) : nullptr;
		if (g_state != nullptr)
		{
			memset(g_state, 0, sizeof(*g_state));
			g_state->Version = 1;
			g_state->Magic = GTR_HOST_MAGIC;
		}
		const HANDLE paint = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(GtrPaintState), GTR_PAINT_MAPPING_NAME);
		g_paint = paint != nullptr ? static_cast<GtrPaintState *>(MapViewOfFile(paint, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GtrPaintState))) : nullptr;
		if (g_paint != nullptr)
		{
			memset(g_paint, 0, sizeof(*g_paint));
			g_paint->Magic = GTR_PAINT_MAGIC;
		}
		log_line("GtrHost started; compositor state %s", g_state != nullptr ? "shared" : "NOT shared");
	}

	/// The numbers of "key":[a, b, ...] in a line of JSON, as the guest and the tools write it. False when the key is missing,
	/// is null, or holds fewer numbers than asked for.
	bool read_numbers(const std::string &line, const char *key, double *out, int count)
	{
		const std::string name = std::string("\"") + key + "\":";
		size_t at = line.find(name);
		if (at == std::string::npos)
			return false;
		at += name.size();
		while (at < line.size() && line[at] == ' ')
			++at;
		const bool list = at < line.size() && line[at] == '[';
		if (list)
			++at;
		else if (count != 1)
			return false;
		const char *cursor = line.c_str() + at;
		for (int i = 0; i < count; ++i)
		{
			char *end = nullptr;
			out[i] = std::strtod(cursor, &end);
			if (end == cursor)
				return false;
			cursor = end;
			while (*cursor == ',' || *cursor == ' ')
				++cursor;
		}
		return true;
	}

	std::string read_string(const std::string &line, const char *key)
	{
		const std::string name = std::string("\"") + key + "\":";
		size_t at = line.find(name);
		if (at == std::string::npos)
			return {};
		at = line.find('"', at + name.size());
		const size_t end = at == std::string::npos ? at : line.find('"', at + 1);
		return end == std::string::npos ? std::string() : line.substr(at + 1, end - at - 1);
	}

	uint64_t cell_key(int column, int row)
	{
		return (static_cast<uint64_t>(static_cast<uint32_t>(column)) << 32) | static_cast<uint32_t>(row);
	}

	bool probe_ground(float x, float y, float fromZ, float &groundZ)
	{
		// A failed probe means the collision there hasn't streamed in; 0 is what an unloaded area answers too
		return natives::GetGroundZFor3dCoord(x, y, fromZ, &groundZ, FALSE, FALSE) && groundZ != 0.0f;
	}

	/// Probes the cells near the character that aren't known yet, nearest first, all of those right around it again, and a
	/// few further ones again.
	void sample_ground(const Vector3 &player)
	{
		// Around the character's feet, where its ground is wanted, rather than GTA's player, which follows it
		Vector3 centre = player;
		centre.z -= kPedOriginHeight;
		if (g_guest.hasCharacter)
		{
			centre.x = float(g_guest.character[0]);
			centre.y = float(g_guest.character[1]);
			centre.z = float(g_guest.character[2]);
		}
		if (g_groundOrder.empty())
		{
			const int reach = static_cast<int>(kGroundRadius / kGroundCell);
			for (int column = -reach; column < reach; ++column)
				for (int row = -reach; row < reach; ++row)
					g_groundOrder.emplace_back(column, row);
			std::sort(g_groundOrder.begin(), g_groundOrder.end(), [](const auto &a, const auto &b) {
				return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
			});
		}

		const int centreColumn = static_cast<int>(std::floor(centre.x / kGroundCell)), centreRow = static_cast<int>(std::floor(centre.y / kGroundCell));
		const float fromZ = centre.z + kPedOriginHeight + kGroundProbeAbove;
		std::string tiles;
		size_t count = 0;
		const auto flush = [&]() {
			if (count == 0)
				return;
			g_link.send_line("{\"t\":\"ground\",\"cell\":" + std::to_string(kGroundCell) + ",\"depth\":" + std::to_string(kGroundDepth) + ",\"tiles\":[" + tiles + "]}");
			tiles.clear();
			count = 0;
		};
		const auto add = [&](int column, int row, float z) {
			char entry[96];
			snprintf(entry, sizeof(entry), "%s%.3f,%.3f,%.3f", count == 0 ? "" : ",", (column + 0.5f) * kGroundCell, (row + 0.5f) * kGroundCell, z);
			tiles += entry;
			if (++count >= kGroundTilesPerMessage)
				flush();
		};

		int probes = 0;
		for (const auto &[dc, dr] : g_groundOrder)
		{
			const int column = centreColumn + dc, row = centreRow + dr;
			const uint64_t key = cell_key(column, row);
			if (g_ground.count(key) != 0)
				continue;
			if (++probes > kGroundProbesPerTick)
				break;
			float z = 0.0f;
			if (!probe_ground((column + 0.5f) * kGroundCell, (row + 0.5f) * kGroundCell, fromZ, z))
				continue;
			g_ground[key] = z;
			add(column, row, z);
		}

		const auto recheck = [&](int column, int row) {
			const auto known = g_ground.find(cell_key(column, row));
			float z = 0.0f;
			if (known == g_ground.end() || !probe_ground((column + 0.5f) * kGroundCell, (row + 0.5f) * kGroundCell, fromZ, z))
				return;
			if (std::fabs(z - known->second) > kGroundChange)
			{
				known->second = z;
				add(column, row, z);
			}
		};
		const int nearCells = static_cast<int>(std::ceil(kGroundNearRadius / kGroundCell));
		for (int dc = -nearCells; dc <= nearCells; ++dc)
			for (int dr = -nearCells; dr <= nearCells; ++dr)
				recheck(centreColumn + dc, centreRow + dr);

		for (int i = 0; i < kGroundRechecksPerTick && !g_groundOrder.empty(); ++i)
		{
			const auto &[dc, dr] = g_groundOrder[g_recheck++ % g_groundOrder.size()];
			const int column = centreColumn + dc, row = centreRow + dr;
			const auto known = g_ground.find(cell_key(column, row));
			float z = 0.0f;
			if (known == g_ground.end() || !probe_ground((column + 0.5f) * kGroundCell, (row + 0.5f) * kGroundCell, fromZ, z))
				continue;
			if (std::fabs(z - known->second) > kGroundChange)
			{
				known->second = z;
				add(column, row, z);
			}
		}
		flush();
	}

	/// Forgets the cells the player has left behind, and has the guest remove their tiles.
	void drop_ground(const Vector3 &player)
	{
		std::string dropped;
		for (auto it = g_ground.begin(); it != g_ground.end();)
		{
			const float x = (static_cast<int32_t>(it->first >> 32) + 0.5f) * kGroundCell, y = (static_cast<int32_t>(it->first & 0xFFFFFFFF) + 0.5f) * kGroundCell;
			if (std::max(std::fabs(x - player.x), std::fabs(y - player.y)) <= kGroundDropRadius)
			{
				++it;
				continue;
			}
			char entry[64];
			snprintf(entry, sizeof(entry), "%s%.3f,%.3f", dropped.empty() ? "" : ",", x, y);
			dropped += entry;
			it = g_ground.erase(it);
		}
		if (!dropped.empty())
			g_link.send_line("{\"t\":\"ground\",\"cell\":" + std::to_string(kGroundCell) + ",\"drop\":[" + dropped + "]}");
	}

	/// Puts the character back on GTA's ground when it has fallen through it: nothing under its feet, and GTA's ground found
	/// above them. A fall from a height has ground under it all the way down, and an area not streamed in has none above either.
	void rescue_character()
	{
		static DWORD last = 0;
		if (!g_guest.hasCharacter || g_guest.character[4] <= 0.0 || GetTickCount() - last < kRescueInterval)
			return;
		const float x = float(g_guest.character[0]), y = float(g_guest.character[1]), feet = float(g_guest.character[2]);
		float z = 0.0f;
		if (probe_ground(x, y, feet + kGroundProbeAbove, z) && z < feet + kGroundProbeAbove)
			return;
		if (!probe_ground(x, y, feet + kRescueFrom, z) || z < feet + kRescueBelow)
			return;
		last = GetTickCount();
		log_line("character fell through at %.1f %.1f %.1f; put back on the ground at %.2f", x, y, feet, z);
		sendf("{\"t\":\"spawn\",\"pos\":[%.3f,%.3f,%.3f],\"move\":true}", x, y, z);
	}

	/// The ground under the player's feet, or a guess below the player's origin while the collision isn't there.
	float feet_height(const Vector3 &player)
	{
		float z = 0.0f;
		return probe_ground(player.x, player.y, player.z + 1.0f, z) ? z : player.z - kPedOriginHeight;
	}

	/// A new connection: the guest's world is placed at the player and filled from nothing.
	void introduce(const Vector3 &player)
	{
		g_origin[0] = std::floor(player.x);
		g_origin[1] = std::floor(player.y);
		g_origin[2] = feet_height(player);
		g_ground.clear();
		g_bricks = 0;
		g_probe = false;
		g_bodies.clear();
		g_guest = GuestState();
		sendf("{\"t\":\"origin\",\"pos\":[%.3f,%.3f,%.3f]}", g_origin[0], g_origin[1], g_origin[2]);
		g_link.send_line("{\"t\":\"clear\"}");
		g_start[0] = player.x;
		g_start[1] = player.y;
		g_start[2] = g_origin[2];
		sendf("{\"t\":\"spawn\",\"pos\":[%.3f,%.3f,%.3f]}", g_start[0], g_start[1], g_start[2]);
		sendf("{\"t\":\"debug\",\"ground\":%s}", g_groundShown ? "true" : "false");
		// The guest is lit as at a plain early afternoon whatever GTA's hour: the compositor then gives its picture the
		// brightness and colour of GTA's picture around it, which a guest already dimmed for night would get twice
		sendf("{\"t\":\"light\",\"clock\":%.1f}", kGuestClock);
		g_connectedAt = GetTickCount();
		g_autoCaptured = false;
		log_line("guest connected; origin %.2f %.2f %.2f", g_origin[0], g_origin[1], g_origin[2]);
		natives::Notify("Vanadium passthrough ~g~connected");
	}

	void place_brick(const Vector3 &player, float heading)
	{
		const float fx = -std::sin(heading * gta::kDegrees), fy = std::cos(heading * gta::kDegrees);
		const float x = player.x + fx * kBrickDistance, y = player.y + fy * kBrickDistance;
		float ground = 0.0f;
		if (!probe_ground(x, y, player.z + kGroundProbeAbove, ground))
			ground = feet_height(player);
		// Each press stacks one more on the same spot until the player moves on
		static float lastX = 0.0f, lastY = 0.0f;
		static int stacked = 0;
		stacked = std::fabs(x - lastX) < 0.3f && std::fabs(y - lastY) < 0.3f ? stacked + 1 : 0;
		lastX = x;
		lastY = y;
		const float *color = kBrickColors[g_bricks % std::size(kBrickColors)];
		sendf("{\"t\":\"brick\",\"id\":\"host%d\",\"pos\":[%.3f,%.3f,%.3f],\"size\":[%.3f,%.3f,%.3f],\"yaw\":%.2f,\"color\":[%.3f,%.3f,%.3f]}",
			g_bricks, x, y, ground + kBrickSize[2] * (stacked + 0.5f), kBrickSize[0], kBrickSize[1], kBrickSize[2], heading, color[0], color[1], color[2]);
		log_line("brick %d at %.2f %.2f %.2f", g_bricks, x, y, ground);
		++g_bricks;
	}

	int health_for_guest(int entity);

	/// Tells the guest of GTA's vehicles, people and objects round a point: the ones that are new or have moved, and now and
	/// then which are still there.
	void send_bodies(Ped player, const Vector3 &centre, int frame)
	{
		struct Found
		{
			int entity;
			int kind;
			float distance;
		};
		static std::vector<Found> found;
		static int handles[1024];
		found.clear();
		const auto gather = [&](int count, int kind) {
			for (int i = 0; i < count; ++i)
			{
				const int entity = handles[i];
				// Not the player, and not what it rides: the character sits inside that
				if (entity == player || (g_ride.state != Ride::OnFoot && entity == g_ride.vehicle) || !natives::DoesEntityExist(entity))
					continue;
				const Vector3 at = natives::GetEntityCoords(entity, FALSE);
				const float dx = at.x - centre.x, dy = at.y - centre.y, dz = at.z - centre.z;
				const float distance = dx * dx + dy * dy + dz * dz;
				if (distance <= kBodyRadius * kBodyRadius)
					found.push_back({entity, kind, distance});
			}
		};
		gather(worldGetAllPeds(handles, int(std::size(handles))), 2);
		gather(worldGetAllVehicles(handles, int(std::size(handles))), 1);
		gather(worldGetAllObjects(handles, int(std::size(handles))), 0);
		// People and vehicles before objects, and the nearer before the farther
		std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) {
			return (a.kind != 0) != (b.kind != 0) ? a.kind != 0 : a.distance < b.distance;
		});

		std::string set, keep;
		size_t kept = 0;
		const bool listing = frame % kBodyKeepInterval == 0;
		std::unordered_map<int, BodySent> still;
		for (const Found &one : found)
		{
			if (kept >= kMaxBodies)
				break;
			Vector3 forward, right, up, position;
			natives::GetEntityMatrix(one.entity, &forward, &right, &up, &position);
			float size[3] = {kPersonSize[0], kPersonSize[1], kPersonSize[2]};
			float centreAt[3] = {position.x, position.y, position.z};
			int health = -1;
			int kind = one.kind;
			if (one.kind == 2 && natives::IsPedInAnyVehicle(one.entity))
			{
				// Riding in a vehicle: the vehicle's box is all the guest has of them, which the place's tools stop at. So they
				// are a box of their own at the vehicle's side, the height of their body and head as they sit, from their seat
				// to just out past the side: where a sword swung at the window, or a shot through it, would meet them. The
				// guest doesn't collide with it, which leaves the vehicle's box as it was
				const Vehicle vehicle = natives::GetVehiclePedIsIn(one.entity, FALSE);
				if (natives::IsEntityDead(one.entity) || vehicle == 0 || (g_ride.state != Ride::OnFoot && vehicle == g_ride.vehicle))
					continue;
				Vector3 low = {}, high = {};
				natives::GetModelDimensions(natives::GetEntityModel(vehicle), &low, &high);
				const Vector3 seat = natives::GetOffsetFromEntityGivenWorldCoords(vehicle, position.x, position.y, position.z);
				const float side = seat.x < 0.0f ? -1.0f : 1.0f;
				const float inner = seat.x - side * kRiderInside, outer = (side < 0.0f ? low.x : high.x) + side * kRiderOutside;
				const Vector3 middle = natives::GetOffsetFromEntityInWorldCoords(vehicle, (inner + outer) * 0.5f, seat.y, seat.z + kRiderRise);
				Vector3 vehicleForward, vehicleRight, vehicleUp, vehicleAt;
				natives::GetEntityMatrix(vehicle, &vehicleForward, &vehicleRight, &vehicleUp, &vehicleAt);
				forward = vehicleForward;
				up = vehicleUp;
				size[0] = std::fabs(outer - inner);
				size[1] = kRiderSize[0];
				size[2] = kRiderSize[1];
				centreAt[0] = middle.x;
				centreAt[1] = middle.y;
				centreAt[2] = middle.z;
				health = health_for_guest(one.entity);
				kind = 3;
			}
			else if (one.kind == 2)
			{
				if (natives::IsEntityDead(one.entity))
					continue;
				health = health_for_guest(one.entity);
				if (!natives::IsPedRagdoll(one.entity))
				{
					// Upright whatever its animation does: only where it faces counts
					const float length = std::hypot(forward.x, forward.y);
					if (length > 0.01f)
					{
						forward.x /= length;
						forward.y /= length;
						forward.z = 0.0f;
						up.x = up.y = 0.0f;
						up.z = 1.0f;
					}
				}
				centreAt[2] -= kPedOriginHeight - kPersonSize[2] * 0.5f;
			}
			else
			{
				if (one.kind == 0 && (!natives::IsEntityVisible(one.entity) || natives::IsEntityAttached(one.entity) || natives::GetEntityCollisionDisabled(one.entity)))
					continue;
				Vector3 low = {}, high = {};
				natives::GetModelDimensions(natives::GetEntityModel(one.entity), &low, &high);
				size[0] = high.x - low.x;
				size[1] = high.y - low.y;
				size[2] = high.z - low.z;
				const float largest = std::max({size[0], size[1], size[2]}), smallest = std::min({size[0], size[1], size[2]});
				if (smallest <= 0.0f || (one.kind == 0 && (largest > kLargestObject || largest < kSmallestObject)))
					continue;
				const float middle[3] = {(low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, (low.z + high.z) * 0.5f};
				centreAt[0] += right.x * middle[0] + forward.x * middle[1] + up.x * middle[2];
				centreAt[1] += right.y * middle[0] + forward.y * middle[1] + up.y * middle[2];
				centreAt[2] += right.z * middle[0] + forward.z * middle[1] + up.z * middle[2];
			}
			++kept;

			BodySent now;
			const float numbers[12] = {centreAt[0], centreAt[1], centreAt[2], forward.x, forward.y, forward.z, up.x, up.y, up.z, size[0], size[1], size[2]};
			std::copy(std::begin(numbers), std::end(numbers), now.numbers);
			now.health = health;
			const auto before = g_bodies.find(one.entity);
			bool changed = before == g_bodies.end() || before->second.health != health;
			for (int i = 0; i < 12 && !changed; ++i)
				changed = std::fabs(before->second.numbers[i] - numbers[i]) > kBodyMoved;
			if (changed)
			{
				// With how fast it goes, for what it runs into in the guest
				const Vector3 velocity = one.kind == 1 ? natives::GetEntityVelocity(one.entity) : Vector3();
				char entry[400];
				snprintf(entry, sizeof(entry), "%s[%d,%d,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%d,%.2f,%.2f,%.2f]", set.empty() ? "" : ",", one.entity, kind,
					numbers[0], numbers[1], numbers[2], numbers[3], numbers[4], numbers[5], numbers[6], numbers[7], numbers[8], numbers[9], numbers[10], numbers[11], health,
					velocity.x, velocity.y, velocity.z);
				set += entry;
				still[one.entity] = now;
			}
			else
			{
				still[one.entity] = before->second;
			}
			if (listing)
				keep += (keep.empty() ? "" : ",") + std::to_string(one.entity);
		}
		// What is no longer near is forgotten here at once, and by the guest at the next listing
		g_bodies.swap(still);
		if (!set.empty() || listing)
			g_link.send_line("{\"t\":\"bodies\",\"set\":[" + set + "]" + (listing ? ",\"keep\":[" + keep + "]" : "") + "}");
	}

	void load_settings()
	{
		FILE *file = nullptr;
		if (g_folder.empty() || fopen_s(&file, (g_folder + "\\" + kSettingsFile).c_str(), "r") != 0 || file == nullptr)
			return;
		float scale = 0.0f;
		if (fscanf_s(file, "lookscale %f", &scale) == 1 && scale >= kLookScaleRange[0] && scale <= kLookScaleRange[1])
			g_lookScale = scale;
		fclose(file);
	}

	void save_settings()
	{
		FILE *file = nullptr;
		if (g_folder.empty() || fopen_s(&file, (g_folder + "\\" + kSettingsFile).c_str(), "w") != 0 || file == nullptr)
			return;
		fprintf(file, "lookscale %.2f\n", g_lookScale);
		fclose(file);
	}

	/// A person's health as the guest keeps it, out of 100: GTA's people are dead at 100 and whole at whatever each was made
	/// with.
	int health_for_guest(int entity)
	{
		const int whole = std::max(natives::GetEntityMaxHealth(entity) - kPedDeadHealth, 1);
		return std::clamp((natives::GetEntityHealth(entity) - kPedDeadHealth) * 100 / whole, 0, 100);
	}

	/// Swings a door open away from whatever pushed it, if the object is a door. GTA's doors don't answer to a force: they
	/// open for GTA's own player walking into them, which the guest's character, stopped by the door's box, never does. A
	/// door is told by its shape, a thin upright slab; for anything that isn't one GTA does nothing with this.
	bool open_door(int entity, const double impulse[3])
	{
		const Hash model = natives::GetEntityModel(entity);
		Vector3 low = {}, high = {};
		natives::GetModelDimensions(model, &low, &high);
		const float across = high.x - low.x, through = high.y - low.y, tall = high.z - low.z;
		const float thin = std::min(across, through), wide = std::max(across, through);
		if (thin > kDoorThickest || wide < kDoorNarrowest || wide > kDoorWidest || tall < kDoorLowest || tall > kDoorTallest)
			return false;
		Vector3 forward, right, up, position;
		natives::GetEntityMatrix(entity, &forward, &right, &up, &position);
		// Its face looks along its thin side
		const Vector3 &face = through <= across ? forward : right;
		const float pushed = float(impulse[0]) * face.x + float(impulse[1]) * face.y;
		const float swing = (pushed >= 0.0f ? 1.0f : -1.0f) * g_doorSign;
		natives::SetStateOfClosestDoorOfType(model, position.x, position.y, position.z, FALSE, swing);
		for (OpenDoor &door : g_doors)
			if (door.entity == entity)
			{
				door.until = GetTickCount() + kDoorOpenMilliseconds;
				return true;
			}
		g_doors.push_back({entity, model, {position.x, position.y, position.z}, GetTickCount() + kDoorOpenMilliseconds});
		log_line("door %d opened, swing %.0f", entity, swing);
		return true;
	}

	/// Puts one of GTA's things where a box of the guest's is: the box's centre, forward and up. A thing's own origin is
	/// not its box's centre, and a person is stood upright whichever way the box was turned.
	void place_entity(int entity, int kind, const double centre[3], const double forward[3], const double up[3])
	{
		if (kind == kEntityPed)
		{
			natives::SetEntityCoordsNoOffset(entity, float(centre[0]), float(centre[1]), float(centre[2]) + kPedOriginHeight - kPersonSize[2] * 0.5f);
			natives::SetEntityHeading(entity, std::atan2(-float(forward[0]), float(forward[1])) / gta::kDegrees);
			natives::SetEntityVelocity(entity, 0.0f, 0.0f, 0.0f);
			return;
		}
		const float f[3] = {float(forward[0]), float(forward[1]), float(forward[2])}, u[3] = {float(up[0]), float(up[1]), float(up[2])};
		const float r[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
		Vector3 low = {}, high = {};
		natives::GetModelDimensions(natives::GetEntityModel(entity), &low, &high);
		const float middle[3] = {(low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, (low.z + high.z) * 0.5f};
		natives::SetEntityCoordsNoOffset(entity, float(centre[0]) - (r[0] * middle[0] + f[0] * middle[1] + u[0] * middle[2]),
			float(centre[1]) - (r[1] * middle[0] + f[1] * middle[1] + u[1] * middle[2]), float(centre[2]) - (r[2] * middle[0] + f[2] * middle[1] + u[2] * middle[2]));
		// The turn as a quaternion, from the matrix whose columns are the thing's right, forward and up
		const float m[3][3] = {{r[0], f[0], u[0]}, {r[1], f[1], u[1]}, {r[2], f[2], u[2]}};
		float x, y, z, w;
		if (const float trace = m[0][0] + m[1][1] + m[2][2]; trace > 0.0f)
		{
			const float scale = std::sqrt(trace + 1.0f) * 2.0f;
			w = 0.25f * scale;
			x = (m[2][1] - m[1][2]) / scale;
			y = (m[0][2] - m[2][0]) / scale;
			z = (m[1][0] - m[0][1]) / scale;
		}
		else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
		{
			const float scale = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
			w = (m[2][1] - m[1][2]) / scale;
			x = 0.25f * scale;
			y = (m[0][1] + m[1][0]) / scale;
			z = (m[0][2] + m[2][0]) / scale;
		}
		else if (m[1][1] > m[2][2])
		{
			const float scale = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
			w = (m[0][2] - m[2][0]) / scale;
			x = (m[0][1] + m[1][0]) / scale;
			y = 0.25f * scale;
			z = (m[1][2] + m[2][1]) / scale;
		}
		else
		{
			const float scale = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
			w = (m[1][0] - m[0][1]) / scale;
			x = (m[0][2] + m[2][0]) / scale;
			y = (m[1][2] + m[2][1]) / scale;
			z = 0.25f * scale;
		}
		natives::SetEntityQuaternion(entity, x, y, z, w);
		natives::SetEntityVelocity(entity, 0.0f, 0.0f, 0.0f);
	}

	/// Hands the people the character has shoved back to GTA's own doings, once they have stepped aside.
	void release_shoved()
	{
		for (auto it = g_shoved.begin(); it != g_shoved.end();)
		{
			if (static_cast<int32_t>(GetTickCount() - it->second) < 0)
			{
				++it;
				continue;
			}
			if (natives::DoesEntityExist(it->first) && !natives::IsEntityDead(it->first) && !natives::IsPedRagdoll(it->first))
				natives::ClearPedTasks(it->first);
			it = g_shoved.erase(it);
		}
	}

	/// Has the people the character struck fight it, once they are up from any fall the blow gave them.
	void fight_back(Ped player)
	{
		for (auto it = g_fightBack.begin(); it != g_fightBack.end();)
		{
			const int entity = it->first;
			if (!natives::DoesEntityExist(entity) || natives::IsEntityDead(entity))
			{
				it = g_fightBack.erase(it);
				continue;
			}
			if (GetTickCount() - it->second < kFightBackAfter || natives::IsPedRagdoll(entity))
			{
				++it;
				continue;
			}
			natives::TaskCombatPed(entity, player);
			it = g_fightBack.erase(it);
		}
	}

	/// Lets go of the things the building tools have let go of: they fall, or drive off, again.
	void release_held()
	{
		for (auto it = g_held.begin(); it != g_held.end();)
		{
			if (static_cast<int32_t>(GetTickCount() - it->second) < 0)
			{
				++it;
				continue;
			}
			if (natives::DoesEntityExist(it->first))
			{
				natives::FreezeEntityPosition(it->first, FALSE);
				natives::ActivatePhysics(it->first);
			}
			it = g_held.erase(it);
		}
	}

	/// Lets the doors that were pushed open fall shut again.
	void close_doors()
	{
		for (size_t i = 0; i < g_doors.size();)
		{
			if (static_cast<int32_t>(GetTickCount() - g_doors[i].until) < 0)
			{
				++i;
				continue;
			}
			natives::SetStateOfClosestDoorOfType(g_doors[i].model, g_doors[i].position[0], g_doors[i].position[1], g_doors[i].position[2], FALSE, 0.0f);
			g_doors.erase(g_doors.begin() + i);
		}
	}

	constexpr uint32_t weather_hash(const char *name);

	/// Arms GTA's player as the character is: the weapon of the tool it holds, or none.
	void arm_player(Ped ped)
	{
		static std::string armedFor = "-";
		if (g_guest.tool != armedFor)
		{
			armedFor = g_guest.tool;
			Hash weapon = weather_hash("WEAPON_UNARMED");
			for (const ToolWeapon &match : kToolWeapons)
				if (g_guest.tool.find(match.word) != std::string::npos)
				{
					weapon = weather_hash(match.weapon);
					break;
				}
			if (weapon != weather_hash("WEAPON_UNARMED"))
				natives::GiveWeaponToPed(ped, weapon);
			natives::SetCurrentPedWeapon(ped, weapon);
			log_line("armed with %s for \"%s\"", weapon == weather_hash("WEAPON_UNARMED") ? "nothing" : "a weapon", armedFor.c_str());
		}
		// GTA's player is unseen, and its weapon must be too
		if (const Entity held = natives::GetCurrentPedWeaponEntityIndex(ped); held != 0)
			natives::SetEntityAlpha(held, 0);
	}

	/// Passes on to the guest's character what GTA's people have done to GTA's player, who stands where the character does: a
	/// punch, a bullet. The player's own health is put back each frame, so GTA doesn't have it die of them; blasts, fire
	/// and being run into it is proof against, since those are the guest's to deal out (its own explosions are set off in
	/// GTA too, and its character is stopped by the vehicles' boxes).
	void forward_harm(Ped ped)
	{
		if (!g_harmReady)
		{
			g_harmReady = true;
			g_playerHealth = 0;
			natives::SetPedMaxHealth(ped, kPlayerHealth);
			natives::SetEntityHealth(ped, kPlayerHealth, 0);
			natives::SetEntityProofs(ped, FALSE, TRUE, TRUE, TRUE, FALSE);
			return;
		}
		const int health = natives::GetEntityHealth(ped);
		if (g_playerHealth == 0)
		{
			// What GTA made of the health asked for: it may not give its player as much
			g_playerHealth = health;
			log_line("the player's health is kept at %d", health);
			return;
		}
		const int lost = g_playerHealth - health;
		if (lost > 0)
		{
			sendf("{\"t\":\"harm\",\"damage\":%d}", lost);
			log_line("the player lost %d", lost);
			natives::SetEntityHealth(ped, g_playerHealth, 0);
		}
	}

	/// Reads the sun file: from the output folder, where a new measurement is put, or else from beside this module.
	void load_sun()
	{
		g_sun.clear();
		char beside[MAX_PATH] = {};
		GetModuleFileNameA(g_module, beside, sizeof(beside));
		if (char *name = strrchr(beside, '\\'); name != nullptr)
			*name = 0;
		for (const std::string &folder : {g_folder, std::string(beside)})
		{
			FILE *file = nullptr;
			if (folder.empty() || fopen_s(&file, (folder + "\\" + kSunFile).c_str(), "r") != 0 || file == nullptr)
				continue;
			char line[256];
			while (fgets(line, sizeof(line), file) != nullptr)
			{
				SunRow row;
				if (line[0] != '#' && sscanf_s(line, "%f %f %f %f %f", &row.hour, &row.direction[0], &row.direction[1], &row.direction[2], &row.shadow) == 5)
					g_sun.push_back(row);
			}
			fclose(file);
			std::sort(g_sun.begin(), g_sun.end(), [](const SunRow &a, const SunRow &b) { return a.hour < b.hour; });
			log_line("%zu rows of sun from %s", g_sun.size(), folder.c_str());
			if (!g_sun.empty())
				return;
		}
		log_line("no sun file: the guest casts no sun shadows");
	}

	/// The hash GTA names its weathers, models and the rest by (Jenkins one-at-a-time, lower case).
	constexpr uint32_t weather_hash(const char *name)
	{
		uint32_t hash = 0;
		for (; *name != 0; ++name)
		{
			hash += static_cast<uint32_t>(*name >= 'A' && *name <= 'Z' ? *name + ('a' - 'A') : *name);
			hash += hash << 10;
			hash ^= hash >> 6;
		}
		hash += hash << 3;
		hash ^= hash >> 11;
		hash += hash << 15;
		return hash;
	}

	void survey_tick()
	{
		Survey &survey = g_survey;
		if (survey.out == nullptr)
			return;
		const auto write = [&survey](const std::string &name, Hash model, bool loaded) {
			Vector3 low = {}, high = {};
			natives::GetModelDimensions(model, &low, &high);
			fprintf(survey.out, "%s %.4f %.4f %.4f %.4f %.4f %.4f %d\n", name.c_str(), low.x, low.y, low.z, high.x, high.y, high.z, loaded ? 1 : 0);
		};
		for (int done = 0; done < kSurveyModelsPerTick && survey.next < survey.names.size(); ++done)
		{
			const std::string &name = survey.names[survey.next];
			const Hash model = weather_hash(name.c_str());
			if (!natives::IsModelInCdimage(model))
			{
				++survey.next;
				continue;
			}
			if (survey.load)
			{
				// One at a time, a model a tick at most
				natives::RequestModel(model);
				const bool loaded = natives::HasModelLoaded(model);
				if (!loaded && ++survey.waited < kSurveyLoadTicks)
					return;
				write(name, model, loaded);
				natives::SetModelAsNoLongerNeeded(model);
				survey.waited = 0;
				++survey.next;
				return;
			}
			write(name, model, false);
			++survey.next;
		}
		if (survey.next >= survey.names.size())
		{
			fclose(survey.out);
			log_line("survey done: %zu models", survey.names.size());
			survey = Survey();
		}
	}

	/// How much of a clear day's shadow a weather leaves. The sun file is measured under a clear sky; these are by eye.
	float weather_shadow(uint32_t weather)
	{
		switch (weather)
		{
		case weather_hash("EXTRASUNNY"):
		case weather_hash("CLEAR"):
			return 1.0f;
		case weather_hash("CLOUDS"):
		case weather_hash("SMOG"):
			return 0.8f;
		case weather_hash("FOGGY"):
		case weather_hash("OVERCAST"):
			return 0.5f;
		case weather_hash("RAIN"):
		case weather_hash("THUNDER"):
		case weather_hash("CLEARING"):
			return 0.2f;
		default:
			return 0.7f;
		}
	}

	/// The same for the weather as it is now, which is on its way from one to another.
	float weather_shadow()
	{
		Hash from = 0, to = 0;
		float share = 0.0f;
		natives::GetCurrWeatherState(&from, &to, &share);
		share = std::clamp(share, 0.0f, 1.0f);
		return weather_shadow(uint32_t(from)) * (1.0f - share) + weather_shadow(uint32_t(to)) * share;
	}

	/// The direction towards whatever lights GTA's world now, and how dark its shadows are. False without a sun file.
	bool sun_now(float direction[3], float &shadow)
	{
		if (g_sun.empty())
			return false;
		const float hour = float(natives::GetClockHours()) + float(natives::GetClockMinutes()) / 60.0f + float(natives::GetClockSeconds()) / 3600.0f;
		// The rows either side of now, round midnight
		size_t after = 0;
		while (after < g_sun.size() && g_sun[after].hour <= hour)
			++after;
		const SunRow &a = g_sun[(after + g_sun.size() - 1) % g_sun.size()], &b = g_sun[after % g_sun.size()];
		float span = b.hour - a.hour, into = hour - a.hour;
		if (span <= 0.0f)
			span += 24.0f;
		if (into < 0.0f)
			into += 24.0f;
		const float t = std::clamp(into / span, 0.0f, 1.0f);
		shadow = a.shadow + (b.shadow - a.shadow) * t;
		// Between the last row of the sun and the first of the moon there is nothing to blend: the nearer row is taken
		const bool together = a.direction[0] * b.direction[0] + a.direction[1] * b.direction[1] + a.direction[2] * b.direction[2] > 0.5f;
		float length = 0.0f;
		for (int i = 0; i < 3; ++i)
		{
			direction[i] = together ? a.direction[i] + (b.direction[i] - a.direction[i]) * t : (t < 0.5f ? a : b).direction[i];
			length += direction[i] * direction[i];
		}
		length = std::sqrt(length);
		for (int i = 0; i < 3 && length > 0.0f; ++i)
			direction[i] /= length;
		return length > 0.0f;
	}

	/// Tells the guest where GTA's sun is, so that it lights its own things from the same side; with no sun (night, indoors)
	/// the guest goes back to a plain afternoon. Now and then, and when the sun has moved.
	void send_sun(Ped ped)
	{
		static DWORD sentAt = 0;
		static float sent[3] = {};
		static bool sentSun = false;
		if (GetTickCount() - sentAt < 1000)
			return;
		float sun[3] = {}, shadow = 0.0f;
		const bool has = sun_now(sun, shadow) && shadow > 0.02f && natives::GetInteriorFromEntity(ped) == 0;
		const bool moved = has && sent[0] * sun[0] + sent[1] * sun[1] + sent[2] * sun[2] < 0.9998f;
		// The first after a connection always goes: the guest may be a new one
		if (has == sentSun && !moved && g_sunSentTo == g_generation)
			return;
		sentAt = GetTickCount();
		sentSun = has;
		g_sunSentTo = g_generation;
		std::copy(sun, sun + 3, sent);
		if (has)
			sendf("{\"t\":\"light\",\"sun\":[%.4f,%.4f,%.4f]}", sun[0], sun[1], sun[2]);
		else
			sendf("{\"t\":\"light\",\"clock\":%.1f}", kGuestClock);
	}

	/// What the sun's shadow takes from red, green and blue, as shares of its darkness, by the hour. Measured from GTA's own
	/// shadows on pavement, against the same pavement in the sun: by day (08:15) 0.44, 0.47 and 0.52 of the lit color, nearly
	/// grey; at sunset (19:50) red down by about half and blue hardly at all, the sun being orange. In between it goes from one
	/// to the other. Sunrise is left as day, not having been measured.
	void sun_tint(float hour, float tint[3])
	{
		constexpr float kDay[3] = {1.05f, 1.0f, 0.92f};
		constexpr float kSunset[3] = {1.8f, 0.8f, 0.27f};
		const float t = std::clamp((hour - kSunsetTintFrom) / (kSunsetTintFull - kSunsetTintFrom), 0.0f, 1.0f);
		const float s = t * t * (3.0f - 2.0f * t);
		for (int i = 0; i < 3; ++i)
			tint[i] = kDay[i] + (kSunset[i] - kDay[i]) * s;
	}

	/// Tells the compositor where GTA's sun is for a camera turned this way, for the shadows the guest casts.
	/// The painted people and props near the camera, as boxes in this tick's camera space, for the compositor to colour. Those
	/// gone from GTA's world are forgotten.
	void publish_paint(const float camera[3], const float rotation[3])
	{
		if (g_paint == nullptr)
			return;
		const gta::Basis basis = gta::basis_from_rotation(rotation[0], rotation[1], rotation[2]);
		const float *axes[3] = {basis.right, basis.up, basis.forward};
		const auto view = [&](const float v[3]) {
			return std::array<float, 3>{axes[0][0] * v[0] + axes[0][1] * v[1] + axes[0][2] * v[2], axes[1][0] * v[0] + axes[1][1] * v[1] + axes[1][2] * v[2],
				axes[2][0] * v[0] + axes[2][1] * v[1] + axes[2][2] * v[2]};
		};
		struct Near
		{
			int entity;
			float distance;
		};
		std::vector<Near> nearby;
		for (auto it = g_painted.begin(); it != g_painted.end();)
		{
			if (!natives::DoesEntityExist(it->first))
			{
				it = g_painted.erase(it);
				continue;
			}
			const Vector3 at = natives::GetEntityCoords(it->first, FALSE);
			const float dx = at.x - camera[0], dy = at.y - camera[1], dz = at.z - camera[2];
			const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (distance < kPaintFarthest)
				nearby.push_back({it->first, distance});
			++it;
		}
		std::sort(nearby.begin(), nearby.end(), [](const Near &a, const Near &b) { return a.distance < b.distance; });
		uint32_t count = 0;
		for (const Near &one : nearby)
		{
			if (count >= GTR_PAINT_BOXES)
				break;
			Vector3 forward, right, up, position, low = {}, high = {};
			natives::GetEntityMatrix(one.entity, &forward, &right, &up, &position);
			natives::GetModelDimensions(natives::GetEntityModel(one.entity), &low, &high);
			// Off the ground it stands on, so the ground round its feet isn't painted with it
			low.z += kPaintAboveGround;
			const float middle[3] = {(low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, (low.z + high.z) * 0.5f};
			const float half[3] = {(high.x - low.x) * 0.5f * kPaintMargin, (high.y - low.y) * 0.5f * kPaintMargin, (high.z - low.z) * 0.5f * kPaintMargin};
			const float r[3] = {right.x, right.y, right.z}, f[3] = {forward.x, forward.y, forward.z}, u[3] = {up.x, up.y, up.z};
			const float centre[3] = {position.x + r[0] * middle[0] + f[0] * middle[1] + u[0] * middle[2] - camera[0],
				position.y + r[1] * middle[0] + f[1] * middle[1] + u[1] * middle[2] - camera[1], position.z + r[2] * middle[0] + f[2] * middle[1] + u[2] * middle[2] - camera[2]};
			GtrPaintBox &box = g_paint->Boxes[count++];
			const auto c = view(centre);
			std::copy(c.begin(), c.end(), box.Centre);
			const float *entityAxes[3] = {r, f, u};
			for (int i = 0; i < 3; ++i)
			{
				const auto a = view(entityAxes[i]);
				for (int j = 0; j < 3; ++j)
					box.Across[i][j] = a[j] / std::max(half[i], 0.01f);
			}
			const auto &color = g_painted[one.entity];
			std::copy(color.begin(), color.end(), box.Color);
			box.Strength = kPaintStrength;
		}
		g_paint->Count = count;
	}

	void publish_sun(Ped ped, const float rotation[3], float fov)
	{
		if (g_state == nullptr)
			return;
		const gta::Basis basis = gta::basis_from_rotation(rotation[0], rotation[1], rotation[2]);
		const float *axes[3] = {basis.right, basis.up, basis.forward};
		// The camera's view and the world's up go out with or without a sun: the paint and the lamps work from them too, and
		// the effect would otherwise go on with the last ones it had, from before the sun set
		for (int i = 0; i < 3; ++i)
			g_state->UpView[i] = axes[i][2];
		g_state->TanHalfFov = std::tan(fov * gta::kDegrees * 0.5f);
		float sun[3] = {}, shadow = 0.0f;
		// No sun indoors
		if (!sun_now(sun, shadow) || natives::GetInteriorFromEntity(ped) != 0)
		{
			g_state->SunShadow = 0.0f;
			return;
		}
		for (int i = 0; i < 3; ++i)
			g_state->SunView[i] = axes[i][0] * sun[0] + axes[i][1] * sun[1] + axes[i][2] * sun[2];
		g_state->SunShadow = shadow * weather_shadow();
		sun_tint(float(natives::GetClockHours()) + float(natives::GetClockMinutes()) / 60.0f, g_state->SunTint);
	}

	/// Starts or ends the bench. With a place, GTA's player is taken there, and brought back afterwards.
	void set_bench(bool on, const double *place)
	{
		const Ped ped = natives::PlayerPedId();
		if (on == g_bench.on)
			return;
		if (on)
		{
			g_bench = Bench();
			g_bench.on = true;
			const Vector3 now = natives::GetEntityCoords(ped, TRUE);
			g_bench.back[0] = now.x;
			g_bench.back[1] = now.y;
			g_bench.back[2] = now.z;
			for (int i = 0; i < 3; ++i)
			{
				g_bench.place[i] = place != nullptr ? float(place[i]) : g_bench.back[i];
				g_bench.offset[i] = kBenchCameraOffset[i];
				g_bench.rotation[i] = kBenchCameraRotation[i];
			}
			log_line("bench at %.1f %.1f %.1f", g_bench.place[0], g_bench.place[1], g_bench.place[2]);
			return;
		}
		if (g_bench.trace != nullptr)
			fclose(g_bench.trace);
		natives::RenderScriptCams(FALSE);
		if (g_bench.camera != 0)
			natives::DestroyCam(g_bench.camera);
		natives::ResetEntityAlpha(ped);
		natives::FreezeEntityPosition(ped, FALSE);
		natives::SetEntityCoordsNoOffset(ped, g_bench.back[0], g_bench.back[1], g_bench.back[2]);
		g_bench = Bench();
		log_line("bench ended");
	}

	/// A frame on the bench: the player kept where it is, and the camera where the tool wants it.
	void bench_tick(Ped ped, int frame)
	{
		Bench &bench = g_bench;
		if (!bench.grounded)
		{
			// Kept at the place, frozen, until GTA has loaded the ground there
			natives::RequestCollisionAtCoord(bench.place[0], bench.place[1], bench.place[2]);
			natives::SetEntityCoordsNoOffset(ped, bench.place[0], bench.place[1], bench.place[2] + kPedOriginHeight);
			float ground = 0.0f;
			if (probe_ground(bench.place[0], bench.place[1], bench.place[2] + 5.0f, ground))
			{
				bench.feet = ground;
				bench.grounded = true;
				natives::SetEntityCoordsNoOffset(ped, bench.place[0], bench.place[1], ground + kPedOriginHeight);
				// How level the ground around is, for a tool that measures shadows on it
				float lowest = ground, highest = ground;
				for (int i = 0; i < 8; ++i)
				{
					float z = ground;
					const float angle = float(i) * 45.0f * gta::kDegrees;
					if (probe_ground(bench.place[0] + std::cos(angle) * 12.0f, bench.place[1] + std::sin(angle) * 12.0f, bench.place[2] + 5.0f, z))
					{
						lowest = std::min(lowest, z);
						highest = std::max(highest, z);
					}
				}
				log_line("bench ground %.3f, within 12 m %.3f..%.3f", ground, lowest, highest);
			}
		}
		if (bench.camera == 0)
		{
			bench.camera = natives::CreateCam("DEFAULT_SCRIPTED_CAMERA");
			natives::SetCamActive(bench.camera, TRUE);
			natives::RenderScriptCams(TRUE);
		}
		natives::FreezeEntityPosition(ped, TRUE);
		natives::SetEntityHeading(ped, 0.0f);
		if (bench.alpha >= 255)
			natives::ResetEntityAlpha(ped);
		else
			natives::SetEntityAlpha(ped, bench.alpha);
		const float feet = bench.grounded ? bench.feet : bench.place[2];
		const float at[3] = {bench.place[0] + bench.offset[0], bench.place[1] + bench.offset[1], feet + bench.offset[2]};
		natives::SetCamCoord(bench.camera, at[0], at[1], at[2]);
		natives::SetCamRot(bench.camera, bench.rotation[0], bench.rotation[1], bench.rotation[2]);
		natives::SetCamFov(bench.camera, bench.fov);
		natives::DisableAllControlActions(0);
		natives::EnableControlAction(0, kControlPause, TRUE);
		natives::EnableControlAction(0, kControlPauseAlternate, TRUE);
		natives::HideHudAndRadarThisFrame();
		natives::InvalidateIdleCam();
		natives::SetMaxWantedLevel(0);
		if (frame % 120 == 0)
			natives::ClearArea(bench.place[0], bench.place[1], feet, kBenchClearRadius);

		if (bench.trace != nullptr)
		{
			// What GTA's shake adds to the camera, in the camera's own terms: it looks north and level for this, so east is
			// its right, north its forward and up its up
			const Vector3 position = natives::GetFinalRenderedCamCoord(), rotation = natives::GetFinalRenderedCamRot(2);
			fprintf(bench.trace, "%lu %.5f %.5f %.5f %.4f %.4f %.4f\n", GetTickCount() - bench.traceStart, position.x - at[0], position.y - at[1],
				position.z - at[2], rotation.x - bench.rotation[0], rotation.y - bench.rotation[1], rotation.z - bench.rotation[2]);
			if (static_cast<int32_t>(GetTickCount() - bench.traceUntil) > 0)
			{
				fclose(bench.trace);
				bench.trace = nullptr;
				log_line("shake written");
			}
		}
	}

	/// Puts the probe beyond the guest's character as its camera sees it, or takes it away.
	void set_probe(bool on)
	{
		g_probe = on && g_guest.hasCharacter && g_guest.hasCamera;
		if (!g_probe)
		{
			g_link.send_line("{\"t\":\"brick\",\"id\":\"probe\",\"remove\":true}");
			return;
		}
		const float yaw = float(g_guest.camera[5]) * gta::kDegrees;
		const float x = float(g_guest.character[0]) - std::sin(yaw) * kProbeDistance, y = float(g_guest.character[1]) + std::cos(yaw) * kProbeDistance;
		float ground = float(g_guest.character[2]);
		probe_ground(x, y, ground + kPedOriginHeight + kGroundProbeAbove, ground);
		g_probeCentre[0] = x;
		g_probeCentre[1] = y;
		g_probeCentre[2] = ground + kProbeSize * 0.5f;
		sendf("{\"t\":\"brick\",\"id\":\"probe\",\"pos\":[%.3f,%.3f,%.3f],\"size\":[%.3f,%.3f,%.3f],\"yaw\":0,\"color\":[0.2,0.8,0.3]}",
			x, y, g_probeCentre[2], kProbeSize, kProbeSize, kProbeSize);
	}

	void capture(const char *reason)
	{
		if (g_state == nullptr || g_folder.empty())
			return;
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s\\capture_%03d.bmp", g_folder.c_str(), ++g_captures);
		strncpy_s(g_state->CapturePath, path, _TRUNCATE);
		g_state->CaptureRequest++;
		log_line("capture %s (%s)", path, reason);
	}

	bool game_in_front()
	{
		DWORD process = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &process);
		return process == GetCurrentProcessId();
	}

	/// Whether a key or mouse button is down, for the game: never while another window is the one being typed into.
	bool is_down(int key)
	{
		return game_in_front() && (GetAsyncKeyState(key) & 0x8000) != 0;
	}

	/// True on the frame a key goes down.
	bool pressed(int key)
	{
		static bool down[256] = {};
		const bool now = is_down(key);
		const bool edge = now && !down[key];
		down[key] = now;
		return edge;
	}

	/// Sends the guest the keys and mouse buttons that went down or came up since the last frame.
	void forward_keys()
	{
		static bool keys[512] = {};
		const auto forward = [](int windows, int roblox) {
			// The keys that drive are GTA's while the character rides
			const bool driving = g_ride.state == Ride::Riding && std::find(std::begin(kDrivingKeys), std::end(kDrivingKeys), roblox) != std::end(kDrivingKeys);
			const bool now = is_down(windows) && !driving;
			if (now != keys[roblox])
			{
				keys[roblox] = now;
				sendf("{\"t\":\"key\",\"code\":%d,\"down\":%s}", roblox, now ? "true" : "false");
			}
		};
		// Roblox numbers a letter as its lower case, and a digit as itself
		for (int letter = 'A'; letter <= 'Z'; ++letter)
			forward(letter, letter + ('a' - 'A'));
		for (int digit = '0'; digit <= '9'; ++digit)
			forward(digit, digit);
		for (const KeyMapping &key : kSpecialKeys)
			forward(key.windows, key.roblox);

		static bool buttons[std::size(kMouseButtons)] = {};
		for (int i = 0; i < int(std::size(kMouseButtons)); ++i)
		{
			const bool now = is_down(kMouseButtons[i]);
			if (now != buttons[i])
			{
				buttons[i] = now;
				sendf("{\"t\":\"button\",\"button\":%d,\"down\":%s}", i, now ? "true" : "false");
			}
		}
	}

	/// Sends the guest the pointer. While the place leaves it free it is GTA's own cursor, shown and read; while the place
	/// holds it still to turn the camera (a right-button drag, first person, shift lock) there is no cursor, and what GTA's
	/// look control reads of the mouse is passed on as movement.
	void forward_pointer(int width, int height)
	{
		float dx = 0.0f, dy = 0.0f;
		if (g_radioWheel)
			return;
		if (g_guest.pointerLock != 0)
		{
			// GTA's cursor is kept where the place's pointer stays meanwhile: left to wander, it would have the pointer
			// jump to wherever it had got to when the place lets go of it
			natives::SetMouseCursorThisFrame();
			if (g_cursorMode == kCursorHidden)
				natives::SetMouseCursorVisible(FALSE);
			natives::SetCursorPosition(g_pointer[0], g_pointer[1]);
			dx = natives::GetDisabledControlNormal(0, kControlLookLeftRight) * g_lookScale;
			dy = natives::GetDisabledControlNormal(0, kControlLookUpDown) * g_lookScale;
			if (g_guest.pointerLock == kPointerLockedToCentre)
				g_pointer[0] = g_pointer[1] = 0.5f;
		}
		else
		{
			// The pointer on screen is the guest's own, part of its interface. GTA's is only followed: switched on, since
			// that is how GTA tracks one, and its arrow hidden. Should the arrow not hide, the pointer can be worked out
			// from the mouse's movement instead (a tool chooses, with the op "cursor")
			float x = g_pointer[0], y = g_pointer[1];
			if (g_cursorMode == kCursorFromMovement)
			{
				x = std::clamp(x + natives::GetDisabledControlNormal(0, kControlLookLeftRight) * g_lookScale / float(std::max(width, 1)), 0.0f, 1.0f);
				y = std::clamp(y + natives::GetDisabledControlNormal(0, kControlLookUpDown) * g_lookScale / float(std::max(height, 1)), 0.0f, 1.0f);
			}
			else
			{
				natives::SetMouseCursorThisFrame();
				if (g_cursorMode == kCursorHidden)
					natives::SetMouseCursorVisible(FALSE);
				x = natives::GetDisabledControlNormal(0, kControlCursorX);
				y = natives::GetDisabledControlNormal(0, kControlCursorY);
			}
			dx = (x - g_pointer[0]) * float(width);
			dy = (y - g_pointer[1]) * float(height);
			g_pointer[0] = x;
			g_pointer[1] = y;
		}
		static float sent[2] = {-1.0f, -1.0f};
		if (dx != 0.0f || dy != 0.0f || sent[0] != g_pointer[0] || sent[1] != g_pointer[1])
		{
			sendf("{\"t\":\"mouse\",\"pos\":[%.5f,%.5f],\"delta\":[%.3f,%.3f]}", g_pointer[0], g_pointer[1], dx, dy);
			sent[0] = g_pointer[0];
			sent[1] = g_pointer[1];
		}

		const bool up = natives::IsDisabledControlJustPressed(0, kControlScrollUp) || natives::IsDisabledControlJustPressed(0, kControlWheelPrevious);
		const bool down = natives::IsDisabledControlJustPressed(0, kControlScrollDown) || natives::IsDisabledControlJustPressed(0, kControlWheelNext);
		if (up != down)
			sendf("{\"t\":\"wheel\",\"delta\":%d}", up ? 1 : -1);
	}

	/// The camera of the newest frame the guest has published, read from the frames' own header: where the guest's camera
	/// was for that frame, as GTA writes a camera. False while the guest publishes nothing new.
	double steady_seconds()
	{
		// The clock the guest stamps its frames with (a steady clock is the performance counter, in both)
		LARGE_INTEGER count, frequency;
		QueryPerformanceCounter(&count);
		QueryPerformanceFrequency(&frequency);
		return double(count.QuadPart) / double(frequency.QuadPart);
	}

	bool newest_frame_camera(int64_t &cameraId, double position[3], float rotation[3], float &fov)
	{
		static HANDLE mapping = nullptr;
		static const GtrFrameHeader *header = nullptr;
		static DWORD nextAttempt = 0, publishedAt = 0;
		static int64_t published = -1;
		if (header == nullptr)
		{
			if (static_cast<int32_t>(GetTickCount() - nextAttempt) < 0)
				return false;
			nextAttempt = GetTickCount() + 1000;
			mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, GTR_FRAME_MAPPING_NAME);
			header = mapping != nullptr ? static_cast<const GtrFrameHeader *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, GTR_FRAME_HEADER_BYTES)) : nullptr;
			if (header == nullptr || header->Magic != GTR_FRAME_MAGIC || header->Version != GTR_FRAME_VERSION)
			{
				if (header != nullptr)
					UnmapViewOfFile(header);
				if (mapping != nullptr)
					CloseHandle(mapping);
				header = nullptr;
				mapping = nullptr;
				return false;
			}
			log_line("reading the guest's frames");
		}

		if (header->Published != published)
		{
			published = header->Published;
			publishedAt = GetTickCount();
		}
		const int32_t index = header->LatestSlot;
		if (GetTickCount() - publishedAt > kStateMilliseconds || index < 0 || index >= GTR_FRAME_SLOTS)
			return false;
		const GtrFrameSlot slot = header->Slots[index];
		if ((slot.Sequence & 1) != 0 || header->Slots[index].Sequence != slot.Sequence)
			return false;

		if (const double age = steady_seconds() - slot.GuestSeconds; age > 0.0 && age < kFrameAgeLimit)
			g_frameAge += (age - g_frameAge) * 0.1;
		cameraId = slot.CameraId;
		std::copy(std::begin(slot.CameraPosition), std::end(slot.CameraPosition), position);
		const float *f = slot.CameraForward;
		rotation[0] = std::asin(std::clamp(f[2], -1.0f, 1.0f)) / gta::kDegrees;
		rotation[1] = std::atan2(-slot.CameraRight[2], slot.CameraUp[2]) / gta::kDegrees;
		rotation[2] = std::atan2(-f[0], f[1]) / gta::kDegrees;
		fov = slot.FovY;
		return true;
	}

	/// Where the character sits in a vehicle: the driver's seat, a little above its bone.
	Vector3 seat_position(Vehicle vehicle, Vector3 &forward, Vector3 &up)
	{
		Vector3 right, position;
		natives::GetEntityMatrix(vehicle, &forward, &right, &up, &position);
		for (const char *bone : kSeatBones)
		{
			if (const int index = natives::GetEntityBoneIndexByName(vehicle, bone); index != -1)
			{
				position = natives::GetWorldPositionOfEntityBone(vehicle, index);
				break;
			}
		}
		position.x += up.x * kRideSeatHeight;
		position.y += up.y * kRideSeatHeight;
		position.z += up.z * kRideSeatHeight;
		return position;
	}

	/// Tells the guest where the seat is and how it is turned, as it is this tick. The guest's picture of the character in
	/// it comes some frames late, but the camera is tied to the seat too (see play), so late or not the character is in the
	/// same place in the picture.
	void send_seat(Vehicle vehicle)
	{
		Vector3 forward, up;
		const Vector3 position = seat_position(vehicle, forward, up);
		sendf("{\"t\":\"seat\",\"pos\":[%.3f,%.3f,%.3f],\"fwd\":[%.4f,%.4f,%.4f],\"up\":[%.4f,%.4f,%.4f]}",
			position.x, position.y, position.z, forward.x, forward.y, forward.z, up.x, up.y, up.z);
	}

	/// Gets the character out of its vehicle, beside it.
	void leave_vehicle(Ped ped)
	{
		if (g_ride.state == Ride::OnFoot)
			return;
		const Vehicle vehicle = g_ride.vehicle;
		g_ride = RideState();
		float out[3] = {};
		if (natives::DoesEntityExist(vehicle))
		{
			const Vector3 beside = natives::GetOffsetFromEntityInWorldCoords(vehicle, -kRideExitSide, 0.0f, 0.0f);
			out[0] = beside.x;
			out[1] = beside.y;
			out[2] = beside.z;
			if (natives::IsPedInVehicle(ped, vehicle))
				natives::TaskLeaveVehicle(ped, vehicle, kLeaveVehicleAtOnce);
			if (natives::GetIsDoorValid(vehicle, kDriverDoor))
				natives::SetVehicleDoorShut(vehicle, kDriverDoor);
		}
		else
		{
			const Vector3 at = natives::GetEntityCoords(ped, TRUE);
			out[0] = at.x;
			out[1] = at.y;
			out[2] = at.z;
		}
		float ground = out[2];
		if (probe_ground(out[0], out[1], out[2] + kGroundProbeAbove, ground))
			out[2] = ground;
		sendf("{\"t\":\"seat\",\"out\":[%.3f,%.3f,%.3f]}", out[0], out[1], out[2]);
		// Carried again (see carry_player), not frozen: a frozen player is a post that GTA's people walk into on the spot
		log_line("left vehicle %d", vehicle);
	}

	/// F by a vehicle: the nearest one the character can reach, its door opened and its driver, if it has one, put out.
	void enter_vehicle(Ped ped)
	{
		if (!g_guest.hasCharacter)
			return;
		static int handles[512];
		const int count = worldGetAllVehicles(handles, int(std::size(handles)));
		Vehicle nearest = 0;
		float nearestDistance = kRideReach * kRideReach;
		for (int i = 0; i < count; ++i)
		{
			if (!natives::DoesEntityExist(handles[i]) || !natives::IsVehicleDriveable(handles[i]))
				continue;
			const Vector3 at = natives::GetEntityCoords(handles[i], FALSE);
			const float dx = at.x - float(g_guest.character[0]), dy = at.y - float(g_guest.character[1]), dz = at.z - float(g_guest.character[2]);
			if (const float distance = dx * dx + dy * dy + dz * dz; distance < nearestDistance)
			{
				nearestDistance = distance;
				nearest = handles[i];
			}
		}
		if (nearest == 0)
			return;
		if (const Ped driver = natives::GetPedInVehicleSeat(nearest, kDriverSeat); driver != 0 && driver != ped)
		{
			natives::TaskLeaveVehicle(driver, nearest, kLeaveVehicleAtOnce);
			natives::TaskSmartFleePed(driver, ped);
		}
		// Not every vehicle has the door (a bike, a quad)
		if (natives::GetIsDoorValid(nearest, kDriverDoor))
			natives::SetVehicleDoorOpen(nearest, kDriverDoor);
		g_ride.state = Ride::GettingIn;
		g_ride.vehicle = nearest;
		g_ride.since = GetTickCount();
		log_line("getting into vehicle %d", nearest);
	}

	/// A frame of the character's ride.
	void ride(Ped ped)
	{
		if (pressed('F'))
		{
			if (g_ride.state == Ride::OnFoot)
				enter_vehicle(ped);
			else
				leave_vehicle(ped);
		}
		if (g_ride.state == Ride::OnFoot)
			return;
		if (!natives::DoesEntityExist(g_ride.vehicle) || (g_ride.state == Ride::Riding && !natives::IsPedInVehicle(ped, g_ride.vehicle)))
		{
			// The vehicle is gone, or GTA has had its player out of it
			leave_vehicle(ped);
			return;
		}
		if (g_ride.state == Ride::GettingIn && GetTickCount() - g_ride.since >= kRideEnterMilliseconds)
		{
			// A driver told to get out may still be in the seat, and GTA puts nobody into a seat that is taken: the player
			// then came straight back out. A driver who hasn't gone is put out
			if (const Ped driver = natives::GetPedInVehicleSeat(g_ride.vehicle, kDriverSeat); driver != 0 && driver != ped)
			{
				natives::ClearPedTasksImmediately(driver);
				natives::TaskSmartFleePed(driver, ped);
				send_seat(g_ride.vehicle);
				return;
			}
			natives::FreezeEntityPosition(ped, FALSE);
			natives::SetPedIntoVehicle(ped, g_ride.vehicle, kDriverSeat);
			if (natives::GetIsDoorValid(g_ride.vehicle, kDriverDoor))
				natives::SetVehicleDoorShut(g_ride.vehicle, kDriverDoor);
			natives::SetVehicleEngineOn(g_ride.vehicle);
			g_ride.state = Ride::Riding;
		}
		send_seat(g_ride.vehicle);
	}

	/// Stops playing the guest: GTA gets its own camera and its player back, standing where the guest's character stood.
	void stop_playing(Ped ped)
	{
		if (g_state != nullptr)
		{
			std::fill(std::begin(g_state->TickCamera), std::end(g_state->TickCamera), 0);
			g_state->SunShadow = 0.0f;
		}
		if (g_paint != nullptr)
			g_paint->Count = 0;
		if (!g_playing)
			return;
		g_playing = false;
		natives::RenderScriptCams(FALSE);
		if (g_camera != 0)
			natives::DestroyCam(g_camera);
		g_camera = 0;
		natives::FreezeEntityPosition(ped, FALSE);
		natives::SetPedCanRagdoll(ped, TRUE);
		natives::SetPedFootstepsEventsEnabled(ped, TRUE);
		natives::StopPedSpeaking(ped, FALSE);
		natives::DisablePedPainAudio(ped, FALSE);
		natives::SetEntityCollision(ped, TRUE, TRUE);
		natives::ResetEntityAlpha(ped);
		g_harmReady = false;
		g_ride = RideState();
		natives::SetPedMaxHealth(ped, 200);
		natives::SetEntityHealth(ped, 200, 0);
		natives::DisplayRadar(TRUE);
		natives::SetEntityProofs(ped, FALSE, FALSE, FALSE, FALSE, FALSE);
		natives::SetMaxWantedLevel(5);
		log_line("stopped playing the guest");
	}

	/// Moves GTA's player to where the guest's character is: by its speed, as GTA moves a player, so that it pushes and is
	/// pushed by GTA's people; put there outright when it has fallen far behind (held by a wall of GTA's the character walked
	/// through, or by a drop the character jumped down).
	void carry_player(Ped ped)
	{
		const double *c = g_guest.character;
		const Vector3 at = natives::GetEntityCoords(ped, TRUE);
		const float dx = float(c[0]) - at.x, dy = float(c[1]) - at.y, dz = float(c[2]) + kPedOriginHeight - at.z;
		if (std::hypot(dx, dy) > kCarryFarthest || std::fabs(dz) > kCarryHighest)
		{
			natives::SetEntityCoordsNoOffset(ped, float(c[0]), float(c[1]), float(c[2]) + kPedOriginHeight);
			natives::SetEntityVelocity(ped, 0.0f, 0.0f, 0.0f);
		}
		else
		{
			// Up and down is gravity's and the ground's
			const Vector3 now = natives::GetEntityVelocity(ped);
			natives::SetEntityVelocity(ped, dx / kCarrySeconds, dy / kCarrySeconds, now.z);
		}
		natives::SetEntityHeading(ped, float(c[3]));
	}

	/// Plays the guest for a frame: its place's camera is GTA's camera, its character is where GTA's player is carried, and
	/// GTA's own controls are off so the keyboard and mouse are the guest's alone.
	void play(Ped ped, int width, int height)
	{
		const bool fresh = GetTickCount() - g_guest.at < kStateMilliseconds;
		if (!g_playing)
		{
			if (!fresh || !g_guest.hasCamera)
				return;
			g_playing = true;
			g_camera = natives::CreateCam("DEFAULT_SCRIPTED_CAMERA");
			natives::SetCamActive(g_camera, TRUE);
			natives::RenderScriptCams(TRUE);
			// Solid, so that GTA's people and traffic have someone standing where the character is, and able to be hurt, so
			// that what they do to it reaches the character (see forward_harm). Not frozen: moved towards the character
			// (see carry_player), so that people who walk into it are pushed and step round it instead of walking on the spot
			// against a post. And not to be knocked over, which would leave it lying while the character walks on
			natives::FreezeEntityPosition(ped, FALSE);
			natives::SetPedCanRagdoll(ped, FALSE);
			// Unseen and unheard: its steps, and its landing as it drops after a jumping character, are no one's the player
			// can see
			natives::SetPedFootstepsEventsEnabled(ped, FALSE);
			natives::SetEntityCollision(ped, TRUE, TRUE);
			natives::SetEntityInvincible(ped, FALSE);
			natives::SetPlayerInvincible(natives::PlayerId(), FALSE);
			natives::SetMaxWantedLevel(5);
			log_line("playing the guest");
		}
		// Faded out, not switched off: GTA goes on casting the shadow of a player it doesn't draw, and that shadow, from
		// GTA's own lights onto GTA's own ground, is the character's
		natives::SetEntityAlpha(ped, 0);
		// And silent: the character is the one being played, and the player's own remarks and cries (bumped into, shoved,
		// shot at) are a voice nobody on screen has. Each tick, since GTA lets them speak again after some of its own events
		natives::StopPedSpeaking(ped, TRUE);
		natives::DisablePedPainAudio(ped, TRUE);
		natives::StopCurrentPlayingAmbientSpeech(ped);
		natives::StopCurrentPlayingSpeech(ped);

		// GTA's camera goes where the guest's was for the newest frame it has published, so that GTA's picture from there
		// and that frame are of the same moment, however late the frame is. The compositor has to show the two together, and
		// GTA draws on another thread, some ticks behind this one. So this tick's number is drawn into the picture as a mark,
		// which comes out with the picture it belongs to, and the frame chosen this tick is filed under that number.
		static unsigned ticks = 0;
		const unsigned tick = ticks++ % GTR_HOST_TICKS;
		int64_t cameraId = 0;
		double position[3];
		float rotation[3], fov = 0.0f;
		const bool framed = newest_frame_camera(cameraId, position, rotation, fov);
		if (g_ride.state == Ride::Riding && fresh && g_guest.hasCamera && g_guest.hasCharacter && natives::DoesEntityExist(g_ride.vehicle))
		{
			// Riding, the vehicle is GTA's and moves at GTA's pace, and a camera put where the guest's frame was drawn from
			// would trail it by the guest's delay, in jerks. So the camera is tied to the seat as it is now, by how the
			// place's camera stands to the character in the guest (which only changes as the player turns it), and the
			// guest's newest frame is shown over it unmatched: character and camera are both tied to the seat there too
			const double *c = g_guest.camera, *at = g_guest.character;
			Vector3 forward, up;
			const Vector3 seat = seat_position(g_ride.vehicle, forward, up);
			const float placed[3] = {seat.x + float(c[0] - at[0]), seat.y + float(c[1] - at[1]), seat.z + float(c[2] - (at[2] + kRideRootAboveFeet))};
			natives::SetCamCoord(g_camera, placed[0], placed[1], placed[2]);
			natives::SetCamRot(g_camera, float(c[3]), float(c[4]), float(c[5]));
			natives::SetCamFov(g_camera, float(c[6]));
			const float turned[3] = {float(c[3]), float(c[4]), float(c[5])};
			publish_sun(ped, turned, float(c[6]));
			publish_paint(placed, turned);
			cameraId = 0;
		}
		else if (framed)
		{
			natives::SetCamCoord(g_camera, float(position[0]), float(position[1]), float(position[2]));
			natives::SetCamRot(g_camera, rotation[0], rotation[1], rotation[2]);
			natives::SetCamFov(g_camera, fov);
			publish_sun(ped, rotation, fov);
			const float placed[3] = {float(position[0]), float(position[1]), float(position[2])};
			publish_paint(placed, rotation);
		}
		else if (fresh && g_guest.hasCamera)
		{
			// No frame to match yet: follow what the guest says of its camera, and show whatever frame is newest
			const double *c = g_guest.camera;
			natives::SetCamCoord(g_camera, float(c[0]), float(c[1]), float(c[2]));
			natives::SetCamRot(g_camera, float(c[3]), float(c[4]), float(c[5]));
			natives::SetCamFov(g_camera, float(c[6]));
			const float turned[3] = {float(c[3]), float(c[4]), float(c[5])};
			publish_sun(ped, turned, float(c[6]));
			const float placed[3] = {float(c[0]), float(c[1]), float(c[2])};
			publish_paint(placed, turned);
		}
		if (g_state != nullptr)
		{
			g_state->TickCamera[tick] = cameraId;
			g_state->MarkOffset = g_markOffset;
		}
		if (cameraId != 0 && width > 0 && height > 0)
		{
			// Two squares in the top left corner: a channel full on for each set bit of the number, then the opposite
			const float w = float(GTR_MARK_PIXELS) / float(width), h = float(GTR_MARK_PIXELS) / float(height);
			const int r = (tick & 1) != 0 ? 255 : 0, g = (tick & 2) != 0 ? 255 : 0, b = (tick & 4) != 0 ? 255 : 0;
			natives::DrawRect(w * 0.5f, h * 0.5f, w, h, r, g, b, 255);
			natives::DrawRect(w * 1.5f, h * 0.5f, w, h, 255 - r, 255 - g, 255 - b, 255);
		}
		if (g_probe)
		{
			const float half = kProbeSize * 0.5f - kProbeInset;
			const float *c = g_probeCentre;
			natives::DrawBox(c[0] - half, c[1] - half, c[2] - half, c[0] + half, c[1] + half, c[2] + half, 255, 0, 255, 255);
		}
		// On foot GTA's player is carried where the character is; riding, it is the other way round, and the guest is told
		// where the seat is
		if (fresh && g_guest.hasCharacter && g_ride.state != Ride::Riding && !natives::IsPedInAnyVehicle(ped))
			carry_player(ped);

		// Everything of GTA's own is off but the pause menu, and, while the character rides, the controls that drive
		natives::DisableAllControlActions(0);
		natives::EnableControlAction(0, kControlPause, TRUE);
		natives::EnableControlAction(0, kControlPauseAlternate, TRUE);
		if (g_ride.state == Ride::Riding)
		{
			for (const int control : kDrivingControls)
				natives::EnableControlAction(0, control, TRUE);
			natives::EnableControlAction(0, kControlRadioWheel, TRUE);
			if (natives::IsDisabledControlPressed(0, kControlRadioWheel) || natives::IsControlPressed(0, kControlRadioWheel))
			{
				g_radioWheel = true;
				natives::EnableControlAction(0, kControlLookLeftRight, TRUE);
				natives::EnableControlAction(0, kControlLookUpDown, TRUE);
			}
			else
			{
				g_radioWheel = false;
			}
		}
		else
			for (const int control : kInteractControls)
				natives::EnableControlAction(0, control, TRUE);
		ride(ped);
		natives::DisplayRadar(FALSE);
		for (const int component : kHiddenHud)
			natives::HideHudComponentThisFrame(component);
		if (natives::GetPlayerWantedLevel(natives::PlayerId()) == 0)
			natives::HideHudComponentThisFrame(kHudWantedStars);
		natives::InvalidateIdleCam();
		forward_keys();
		forward_pointer(width, height);
		forward_harm(ped);
		if (g_ride.state == Ride::OnFoot)
			arm_player(ped);
		// The police come for what the character does. At one star they arrest, and GTA's arrest is a scene of its own that
		// takes its player away: from the first star they shoot instead
		if (natives::GetPlayerWantedLevel(natives::PlayerId()) == 1)
			natives::SetPlayerWantedLevel(natives::PlayerId(), 2);
	}

	/// What the guest and the tools beside it say to the script.
	void handle(const std::string &line)
	{
		const std::string type = read_string(line, "t");
		if (type == "state")
		{
			const bool wasAlive = g_guest.hasCharacter && g_guest.character[4] > 0.0;
			g_guest.hasCharacter = read_numbers(line, "char", g_guest.character, 5);
			// The character's death ends the chase, as the player's own would
			if (wasAlive && g_guest.hasCharacter && g_guest.character[4] <= 0.0)
				natives::ClearPlayerWantedLevel(natives::PlayerId());
			g_guest.hasCamera = read_numbers(line, "cam", g_guest.camera, 7);
			g_guest.tool = read_string(line, "tool");
			double lock = 0.0;
			g_guest.pointerLock = read_numbers(line, "lock", &lock, 1) ? int(lock) : 0;
			g_guest.at = GetTickCount();
		}
		else if (type == "explosion")
		{
			double position[3], radius = 0.0;
			if (!read_numbers(line, "pos", position, 3))
				return;
			read_numbers(line, "radius", &radius, 1);
			double pressure = 1.0;
			read_numbers(line, "pressure", &pressure, 1);
			if (pressure <= 0.0)
			{
				// One that pushes nothing, and in the place hurts nothing (the Delete tool's puff as it takes a part away): seen and
				// heard, but harmless, and no one's, so that it neither sets off the vehicles round it nor brings the police
				// Its full size and shake, as an owned one's (a damage scale of 0 shrank the fire to a wisp): only GTA's no-damage
				// switch keeps it harmless
				natives::AddExplosion(float(position[0]), float(position[1]), float(position[2]), kExplosionGrenade, 1.0f, TRUE, FALSE, 1.0f, TRUE);
				log_line("harmless explosion at %.1f %.1f %.1f", position[0], position[1], position[2]);
				return;
			}
			// The player's own, so that GTA's people and police know whose it was
			natives::AddOwnedExplosion(natives::PlayerPedId(), float(position[0]), float(position[1]), float(position[2]),
				radius >= kLargeBlastRadius ? kExplosionRocket : kExplosionGrenade, 1.0f, 1.0f);
			g_ownBlastAt = GetTickCount();
			log_line("explosion at %.1f %.1f %.1f, radius %.1f", position[0], position[1], position[2], radius);
		}
		else if (type == "hurt")
		{
			// A tool of the place's has hurt one of GTA's people: GTA's player did it, as far as that person and those who
			// saw it are concerned
			double id = 0.0, damage = 0.0;
			if (!read_numbers(line, "id", &id, 1) || !read_numbers(line, "damage", &damage, 1))
				return;
			const int entity = int(id);
			if (!natives::DoesEntityExist(entity) || natives::GetEntityType(entity) != kEntityPed || natives::IsEntityDead(entity))
				return;
			// The damage is out of 100, as the guest keeps a person's health
			const Ped player = natives::PlayerPedId();
			const int whole = std::max(natives::GetEntityMaxHealth(entity) - kPedDeadHealth, 1);
			const int health = natives::GetEntityHealth(entity) - std::max(1, int(damage * whole / 100.0 + 0.5));
			// At 100 GTA's people are dying but not yet dead: a tool that has taken the last of the health has killed
			natives::SetEntityHealth(entity, health > kPedDeadHealth ? health : 0, player);
			if (health > kPedDeadHealth)
			{
				// Still standing: staggered by a hard blow, and it fights whoever struck it once it is up (see fight_back)
				if (damage >= kStaggerDamage && !natives::IsPedRagdoll(entity) && !natives::IsPedInAnyVehicle(entity))
				{
					natives::SetPedCanRagdoll(entity, TRUE);
					natives::SetPedToRagdoll(entity, 900);
				}
				g_fightBack.emplace(entity, GetTickCount());
			}
			log_line("hurt %d by %.0f of 100, health now %d of %d", entity, damage, health, whole + kPedDeadHealth);
		}
		else if (type == "move" || type == "copy")
		{
			// One of the place's building tools has dragged the box of one of GTA's things, or left a copy of it somewhere:
			// the thing is put there, or one like it is made there
			double id = 0.0, centre[3], forward[3], up[3];
			if (!read_numbers(line, "id", &id, 1) || !read_numbers(line, "pos", centre, 3) || !read_numbers(line, "fwd", forward, 3) || !read_numbers(line, "up", up, 3))
				return;
			int entity = int(id);
			if (!natives::DoesEntityExist(entity))
				return;
			const int kind = natives::GetEntityType(entity);
			if (type == "copy")
			{
				const Hash model = natives::GetEntityModel(entity);
				const float heading = std::atan2(-float(forward[0]), float(forward[1])) / gta::kDegrees;
				const int made = kind == kEntityVehicle ? natives::CreateVehicle(model, float(centre[0]), float(centre[1]), float(centre[2]), heading)
					: kind == kEntityPed ? natives::ClonePed(entity) : natives::CreateObjectNoOffset(model, float(centre[0]), float(centre[1]), float(centre[2]));
				log_line("copied %d as %d", entity, made);
				if (made == 0)
					return;
				entity = made;
			}
			place_entity(entity, kind, centre, forward, up);
			if (type == "copy")
			{
				natives::ActivatePhysics(entity);
				natives::SetEntityAsNoLongerNeeded(&entity);
			}
			else
			{
				// Held where the tool has it until the tool lets go
				natives::FreezeEntityPosition(entity, TRUE);
				g_held[entity] = GetTickCount() + kHeldMilliseconds;
			}
		}
		else if (type == "delete")
		{
			double id = 0.0;
			if (!read_numbers(line, "id", &id, 1))
				return;
			int entity = int(id);
			if (!natives::DoesEntityExist(entity) || entity == natives::PlayerPedId() || (g_ride.state != Ride::OnFoot && entity == g_ride.vehicle))
				return;
			natives::SetEntityAsMissionEntity(entity);
			natives::DeleteEntity(&entity);
			log_line("deleted %d", int(id));
		}
		else if (type == "probes")
		{
			// The guest's fast things: rockets, balls, pellets. The guest has ground and bodies only near its character, and of
			// GTA's walls nothing at all, so for each of these GTA's own world is asked what lies in its way, and the guest
			// puts a wall of its own there for it to hit
			double count = 0.0;
			static double numbers[kMostProbes * 7];
			if (!read_numbers(line, "n", &count, 1) || count < 1.0 || count > kMostProbes || !read_numbers(line, "list", numbers, int(count) * 7))
				return;
			std::string walls;
			for (int i = 0; i < int(count); ++i)
			{
				const double *p = numbers + i * 7;
				const float speed = float(std::sqrt(p[4] * p[4] + p[5] * p[5] + p[6] * p[6]));
				if (speed < 0.1f)
					continue;
				// As far as it goes in the time the answer takes to matter, and a little way at the least
				const float reach = std::max(speed * kProbeSeconds, kProbeLeast) / speed;
				Vector3 at, normal;
				Entity met = 0;
				if (!natives::LineHits(float(p[1]), float(p[2]), float(p[3]), float(p[1] + p[4] * reach), float(p[2] + p[5] * reach), float(p[3] + p[6] * reach),
						natives::PlayerPedId(), at, normal, met))
					continue;
				// One of GTA's things the guest has a box for: the guest's thing is to hit that box, which hurts or pushes it.
				// A wall put in front of it (GTA's people are wider than their box) took the blow instead, and hurt no one
				if (met != 0 && g_bodies.count(met) != 0)
					continue;
				char entry[160];
				snprintf(entry, sizeof(entry), "%s%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", walls.empty() ? "" : ",", int(p[0]), at.x, at.y, at.z, normal.x, normal.y, normal.z);
				walls += entry;
			}
			if (!walls.empty())
				g_link.send_line("{\"t\":\"walls\",\"list\":[" + walls + "]}");
		}
		else if (type == "paint")
		{
			// The paintball gun has hit one of GTA's things: a vehicle is painted the ball's colour, body and trim; a person or a
			// prop, which GTA can't colour, is coloured in the compositor's picture (publish_paint)
			double id = 0.0, color[3];
			if (!read_numbers(line, "id", &id, 1) || !read_numbers(line, "color", color, 3))
				return;
			const int entity = int(id);
			if (!natives::DoesEntityExist(entity))
				return;
			const int r = std::clamp(int(color[0]), 0, 255), g = std::clamp(int(color[1]), 0, 255), b = std::clamp(int(color[2]), 0, 255);
			if (natives::GetEntityType(entity) == kEntityVehicle)
			{
				natives::SetVehicleCustomPrimaryColour(entity, r, g, b);
				natives::SetVehicleCustomSecondaryColour(entity, r, g, b);
			}
			else
			{
				g_painted[entity] = {r / 255.0f, g / 255.0f, b / 255.0f};
			}
			log_line("%d painted %d %d %d", entity, r, g, b);
		}
		else if (type == "blocked")
		{
			double id = 0.0, gap = 0.0;
			if (!read_numbers(line, "id", &id, 1) || !read_numbers(line, "gap", &gap, 1))
				return;
			Blocked &blocked = g_blocked[int(id)];
			if (blocked.heard == 0)
				blocked.lastTick = GetTickCount();
			blocked.heard = GetTickCount();
			blocked.gap = float(gap);
		}
		else if (type == "crash")
		{
			// One of GTA's vehicles has run into something of the guest's: it is slowed as that would slow it, and dented
			double id = 0.0, change[3], at[3], hard = 0.0;
			if (!read_numbers(line, "id", &id, 1) || !read_numbers(line, "dv", change, 3) || !read_numbers(line, "at", at, 3))
				return;
			read_numbers(line, "hard", &hard, 1);
			const int entity = int(id);
			if (!natives::DoesEntityExist(entity) || natives::GetEntityType(entity) != kEntityVehicle)
				return;
			const Vector3 now = natives::GetEntityVelocity(entity);
			natives::SetEntityVelocity(entity, now.x + float(change[0]), now.y + float(change[1]), now.z + float(change[2]));
			if (hard >= kCrashDentSpeed)
			{
				const Vector3 where = natives::GetOffsetFromEntityGivenWorldCoords(entity, float(at[0]), float(at[1]), float(at[2]));
				natives::SetVehicleDamage(entity, where.x, where.y, where.z, std::min(float(hard) * kCrashDentPerSpeed, kCrashDentMost), kCrashDentRadius);
			}
			log_line("vehicle %d ran into the guest's at %.1f m/s", entity, hard);
		}
		else if (type == "impulse")
		{
			// Something of the guest's has run into one of GTA's things: it is pushed as that would push it
			double id = 0.0, impulse[3];
			if (!read_numbers(line, "id", &id, 1) || !read_numbers(line, "impulse", impulse, 3))
				return;
			const int entity = int(id);
			if (!natives::DoesEntityExist(entity))
				return;
			const int type = natives::GetEntityType(entity);
			if (type == kEntityObject && open_door(entity, impulse))
				return;
			if (type == kEntityPed && line.find("\"walk\":true") != std::string::npos)
			{
				// The character walking into a person: they step out of its way, on their own feet, the way it walks. (Their
				// speed set outright had them jump.) Told again no oftener than this, so the step isn't started over and over
				static std::unordered_map<int, DWORD> told;
				const float length = float(std::hypot(impulse[0], impulse[1]));
				if (length > 0.0f && !natives::IsPedRagdoll(entity) && GetTickCount() - told[entity] >= kShoveMilliseconds)
				{
					told[entity] = GetTickCount();
					const Vector3 at = natives::GetEntityCoords(entity, TRUE);
					const float dx = float(impulse[0]) / length, dy = float(impulse[1]) / length;
					natives::TaskGoStraightToCoord(entity, at.x + dx * kShoveStep, at.y + dy * kShoveStep, at.z, kShoveSpeed, kShoveMilliseconds * 3,
						std::atan2(dx, -dy) / gta::kDegrees);
					// Given back to GTA once the step is done: a person left with the task walked on the spot
					g_shoved[entity] = GetTickCount() + kShoveMilliseconds * 2;
				}
				return;
			}
			const float mass = type == kEntityVehicle ? kBodyMass[1] / kVehicleKnock : kBodyMass[type == kEntityPed ? 2 : 0];
			float push[3] = {float(impulse[0]) / mass, float(impulse[1]) / mass, float(impulse[2]) / mass};
			const float speed = std::sqrt(push[0] * push[0] + push[1] * push[1] + push[2] * push[2]);
			if (speed > kLargestPush)
				for (float &axis : push)
					axis *= kLargestPush / speed;
			if (type == kEntityPed && speed >= kRagdollPush)
			{
				natives::SetPedCanRagdoll(entity, TRUE);
				natives::SetPedToRagdoll(entity, 2000);
			}
			natives::ApplyForceToEntityCentreOfMass(entity, push[0], push[1], push[2]);
			log_line("pushed %d by %.1f m/s", entity, std::min(speed, kLargestPush));
		}
		else if (type == "host")
		{
			// From a tool directing the session
			const std::string op = read_string(line, "op");
			double numbers[3] = {};
			if (op == "time" && read_numbers(line, "clock", numbers, 2))
				natives::SetClockTime(int(numbers[0]), int(numbers[1]), 0);
			else if (op == "weather")
				natives::SetWeatherTypeNowPersist(read_string(line, "name").c_str());
			else if (op == "explode" && read_numbers(line, "pos", numbers, 3))
				natives::AddExplosion(float(numbers[0]), float(numbers[1]), float(numbers[2]), kExplosionGrenade, 0.0f, TRUE, FALSE, 1.0f, TRUE);
			else if (op == "lookscale" && read_numbers(line, "value", numbers, 1))
				g_lookScale = float(numbers[0]);
			else if (op == "probe" && read_numbers(line, "on", numbers, 1))
				set_probe(numbers[0] != 0.0);
			else if (op == "markoffset" && read_numbers(line, "value", numbers, 1))
				g_markOffset = int(numbers[0]);
			else if (op == "sunreload")
				load_sun();
			else if (op == "lockstep" && read_numbers(line, "on", numbers, 1) && g_state != nullptr)
			{
				g_state->FreeRunning = numbers[0] != 0.0 ? 0 : 1;
				log_line("the compositor %s", g_state->FreeRunning != 0 ? "lets the game draw as fast as it likes" : "waits for the guest's frames");
			}
			else if (op == "cursor" && read_numbers(line, "mode", numbers, 1))
				g_cursorMode = int(numbers[0]);
			else if (op == "wanted" && read_numbers(line, "by", numbers, 1))
			{
				const int level = std::clamp(natives::GetPlayerWantedLevel(natives::PlayerId()) + int(numbers[0]), 0, 5);
				if (level == 0)
					natives::ClearPlayerWantedLevel(natives::PlayerId());
				else
					natives::SetPlayerWantedLevel(natives::PlayerId(), level);
			}
			else if (op == "wanted" && read_numbers(line, "level", numbers, 1))
			{
				if (numbers[0] <= 0.0)
					natives::ClearPlayerWantedLevel(natives::PlayerId());
				else
					natives::SetPlayerWantedLevel(natives::PlayerId(), std::min(int(numbers[0]), 5));
			}
			else if (op == "doorsign" && read_numbers(line, "value", numbers, 1))
				g_doorSign = numbers[0] < 0.0 ? -1.0f : 1.0f;
			else if (op == "showbodies" && read_numbers(line, "on", numbers, 1))
			{
				g_bodiesShown = numbers[0] != 0.0;
				sendf("{\"t\":\"debug\",\"bodies\":%s}", g_bodiesShown ? "true" : "false");
			}
			else if (op == "modeldims" && g_survey.out == nullptr)
			{
				// The names are in models.txt in the output folder, one a line; the sizes go to modeldims.txt
				FILE *names = nullptr;
				if (fopen_s(&names, (g_folder + "\\models.txt").c_str(), "r") == 0 && names != nullptr)
				{
					char name[256];
					while (fgets(name, sizeof(name), names) != nullptr)
					{
						std::string trimmed(name);
						while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == ' '))
							trimmed.pop_back();
						if (!trimmed.empty())
							g_survey.names.push_back(trimmed);
					}
					fclose(names);
					g_survey.load = read_numbers(line, "load", numbers, 1) && numbers[0] != 0.0;
					g_survey.out = _fsopen((g_folder + "\\modeldims.txt").c_str(), "w", _SH_DENYNO);
				}
			}
			else if (op == "bench" && read_numbers(line, "on", numbers, 1))
			{
				const bool on = numbers[0] != 0.0;
				set_bench(on, read_numbers(line, "pos", numbers, 3) ? numbers : nullptr);
			}
			else if (op == "benchcam" && g_bench.on)
			{
				double values[3];
				if (read_numbers(line, "offset", values, 3))
					std::transform(values, values + 3, g_bench.offset, [](double value) { return float(value); });
				if (read_numbers(line, "rot", values, 3))
					std::transform(values, values + 3, g_bench.rotation, [](double value) { return float(value); });
				if (read_numbers(line, "fov", values, 1))
					g_bench.fov = float(values[0]);
			}
			else if (op == "pedalpha" && g_bench.on && read_numbers(line, "value", numbers, 1))
				g_bench.alpha = int(numbers[0]);
			else if (op == "shaketrace" && g_bench.on && g_bench.camera != 0 && g_bench.trace == nullptr)
			{
				// Shakes the bench's camera as GTA shakes one, and writes what that does to it to shake_<name>.txt
				const std::string name = read_string(line, "name");
				double seconds = 3.0;
				numbers[0] = 1.0;
				read_numbers(line, "amp", numbers, 1);
				read_numbers(line, "seconds", &seconds, 1);
				g_bench.trace = _fsopen((g_folder + "\\shake_" + name + ".txt").c_str(), "w", _SH_DENYNO);
				g_bench.traceStart = GetTickCount();
				g_bench.traceUntil = g_bench.traceStart + DWORD(seconds * 1000.0);
				natives::ShakeCam(g_bench.camera, name.c_str(), float(numbers[0]));
			}
			log_line("tool: %.200s", line.c_str());
		}
		else
		{
			log_line("guest: %.200s", line.c_str());
		}
	}

	DWORD random_between(const DWORD range[2])
	{
		return std::uniform_int_distribution<DWORD>(range[0], range[1])(g_random);
	}

	/// Drivers with the guest's things in their way: slowed to a stop short of them, impatient, and in the end pushing through.
	/// The driver's own task goes on throughout, and is only held back, so it drives off as it was once the way is clear.
	void hold_blocked()
	{
		const DWORD now = GetTickCount();
		for (auto it = g_blocked.begin(); it != g_blocked.end();)
		{
			const int vehicle = it->first;
			Blocked &blocked = it->second;
			const bool there = natives::DoesEntityExist(vehicle) && natives::GetEntityType(vehicle) == kEntityVehicle;
			const Ped driver = there ? natives::GetPedInVehicleSeat(vehicle, -1) : 0;
			// The player's own vehicle, or the character's, is the player's to drive into what they like
			if (now - blocked.heard > kBlockedForget || driver == 0 || natives::IsPedAPlayer(driver) || natives::IsEntityDead(driver) ||
				(g_ride.state != Ride::OnFoot && vehicle == g_ride.vehicle))
			{
				if (there && blocked.fedUp)
					log_line("vehicle %d is past the guest's things", vehicle);
				it = g_blocked.erase(it);
				continue;
			}
			const float seconds = std::min(float(now - blocked.lastTick) / 1000.0f, 0.1f);
			blocked.lastTick = now;
			Vector3 forward, right, up, position;
			natives::GetEntityMatrix(vehicle, &forward, &right, &up, &position);
			const Vector3 velocity = natives::GetEntityVelocity(vehicle);
			const float ahead = velocity.x * forward.x + velocity.y * forward.y + velocity.z * forward.z;
			if (blocked.stoppedAt == 0 && ahead < kBlockedStill)
			{
				blocked.stoppedAt = now;
				blocked.fedUpAt = now + random_between(kBlockedPatience);
				blocked.hornAt = now + random_between(kBlockedHornApart) / 2;
			}
			if (!blocked.fedUp && blocked.stoppedAt != 0 && now >= blocked.fedUpAt)
			{
				blocked.fedUp = true;
				natives::StartVehicleHorn(vehicle, int(kBlockedFedUpHorn));
				log_line("vehicle %d has lost patience with the guest's things and pushes through", vehicle);
			}
			// As fast as it could still stop short of them from, braking firmly; or, out of patience, a slow roll into them
			const float allowed = blocked.fedUp ? kBlockedPushSpeed : std::sqrt(2.0f * kBlockedBraking * std::max(blocked.gap - kBlockedStopShort, 0.0f));
			if (ahead > allowed)
			{
				const float slowed = std::max(allowed, ahead - kBlockedBraking * seconds);
				const float less = ahead - slowed;
				natives::SetEntityVelocity(vehicle, velocity.x - forward.x * less, velocity.y - forward.y * less, velocity.z - forward.z * less);
				if (!blocked.fedUp)
					natives::SetVehicleBrakeLights(vehicle, TRUE);
			}
			if (!blocked.fedUp && blocked.stoppedAt != 0 && now >= blocked.hornAt)
			{
				natives::StartVehicleHorn(vehicle, int(random_between(kBlockedHorn)));
				blocked.hornAt = now + random_between(kBlockedHornApart);
			}
			++it;
		}
	}

	double now_milliseconds()
	{
		static const double perMillisecond = [] {
			LARGE_INTEGER frequency;
			QueryPerformanceFrequency(&frequency);
			return double(frequency.QuadPart) / 1000.0;
		}();
		LARGE_INTEGER counter;
		QueryPerformanceCounter(&counter);
		return double(counter.QuadPart) / perMillisecond;
	}

	// Where the script's time goes, in milliseconds summed over the ticks since the last "frame" line, which reports them a
	// tick each. The script runs on GTA's game thread, so what it takes is taken from GTA's frame
	struct TickCosts
	{
		double heard = 0.0, play = 0.0, ground = 0.0, bodies = 0.0, rest = 0.0, frame = 0.0;
		int ticks = 0;
	} g_costs;

	void tick()
	{
		static int frame = 0;
		static double lastStart = 0.0;
		++frame;
		const double start = now_milliseconds();
		if (lastStart > 0.0)
			g_costs.frame += start - lastStart;
		lastStart = start;
		if (g_state != nullptr)
			g_state->Heartbeat++;
		const Ped ped = natives::PlayerPedId();

		for (const std::string &line : g_link.poll())
			handle(line);
		survey_tick();

		if (pressed(VK_F7))
		{
			g_enabled = !g_enabled;
			natives::Notify(g_enabled ? "Vanadium passthrough ~g~on" : "Vanadium passthrough ~r~off");
		}
		const bool hidden = natives::IsPauseMenuActive() || natives::IsScreenFadedOut() || natives::IsCutsceneActive() || natives::IsPlayerSwitchInProgress();
		const bool on = g_enabled && g_link.connected() && !g_bench.on;
		const float nearClip = natives::GetFinalRenderedCamNearClip(), farClip = natives::GetFinalRenderedCamFarClip();
		if (g_state != nullptr)
		{
			// GTA draws its HUD before the compositor lays the guest over the picture: while the radio wheel is up the guest
			// is put away, or it would cover the wheel
			g_state->Active = on && !hidden && !g_radioWheel ? 1 : 0;
			g_state->NearClip = nearClip;
			g_state->FarClip = farClip;
		}
		if (!on)
		{
			stop_playing(ped);
			if (g_bench.on)
				bench_tick(ped, frame);
			return;
		}

		if (g_link.generation() != g_generation)
		{
			g_generation = g_link.generation();
			stop_playing(ped);
			introduce(natives::GetEntityCoords(ped, TRUE));
		}

		const double heard = now_milliseconds();
		int width = 0, height = 0;
		natives::GetActualScreenResolution(&width, &height);
		play(ped, width, height);
		const double played = now_milliseconds();
		const Vector3 player = natives::GetEntityCoords(ped, TRUE);

		// The guest draws its picture from its place's own camera, at the size of GTA's picture. Sent every frame, small as it
		// is: hearing from the host is how the guest knows the keys it holds for it are still held
		const float fov = natives::GetFinalRenderedCamFov();
		sendf("{\"t\":\"view\",\"w\":%d,\"h\":%d}", width, height);

		// Riding, the character is held in its seat and stands on nothing, and at a vehicle's speed the ground would be all
		// new every second
		if (g_ride.state != Ride::Riding)
		{
			sample_ground(player);
			if (frame % kGroundDropInterval == 0)
				drop_ground(player);
			rescue_character();
		}
		const double grounded = now_milliseconds();
		send_bodies(ped, player, frame);
		hold_blocked();
		const double bodied = now_milliseconds();
		release_held();
		release_shoved();
		fight_back(ped);
		send_sun(ped);
		close_doors();

		for (const int key : {VK_PRIOR, VK_NEXT})
		{
			if (!pressed(key))
				continue;
			g_lookScale = std::clamp(key == VK_PRIOR ? g_lookScale * kLookScaleStep : g_lookScale / kLookScaleStep, kLookScaleRange[0], kLookScaleRange[1]);
			save_settings();
			char text[64];
			snprintf(text, sizeof(text), "Camera sensitivity ~y~%.0f", g_lookScale);
			natives::Notify(text);
		}
		if (pressed(VK_F6))
		{
			// 0, +1, -1: a mark can only be a tick early or late
			g_markOffset = g_markOffset == 0 ? 1 : g_markOffset == 1 ? -1 : 0;
			char text[64];
			snprintf(text, sizeof(text), "Vanadium frame offset ~y~%d", g_markOffset);
			natives::Notify(text);
			log_line("mark offset %d", g_markOffset);
		}

		if (pressed(VK_F8))
			place_brick(player, natives::GetEntityHeading(ped));
		if (pressed(VK_F9))
		{
			g_groundShown = !g_groundShown;
			sendf("{\"t\":\"debug\",\"ground\":%s}", g_groundShown ? "true" : "false");
		}
		if (pressed(VK_F10))
			sendf("{\"t\":\"spawn\",\"pos\":[%.3f,%.3f,%.3f]}", g_start[0], g_start[1], g_start[2]);
		if (pressed(VK_F11))
			capture("F11");
		if (!g_autoCaptured && !hidden && GetTickCount() - g_connectedAt > kAutoCaptureMilliseconds)
		{
			g_autoCaptured = true;
			capture("a few seconds after connecting");
		}

		const double end = now_milliseconds();
		g_costs.heard += heard - start;
		g_costs.play += played - heard;
		g_costs.ground += grounded - played;
		g_costs.bodies += bodied - grounded;
		g_costs.rest += end - bodied;
		++g_costs.ticks;
		if (frame % 600 == 1 && g_costs.ticks > 0)
		{
			const double ticks = g_costs.ticks;
			log_line("script costs, ms a tick: heard %.2f play %.2f ground %.2f bodies %.2f rest %.2f; GTA's frame %.2f", g_costs.heard / ticks,
				g_costs.play / ticks, g_costs.ground / ticks, g_costs.bodies / ticks, g_costs.rest / ticks, g_costs.frame / ticks);
			g_costs = TickCosts();
		}
		if (frame % 600 == 1)
			log_line("frame %d: player %.1f %.1f %.1f, camera fov %.1f, clip %.3f..%.0f, screen %dx%d, %zu ground cells, clock %02d:%02d, %s",
				frame, player.x, player.y, player.z, fov, nearClip, farClip, width, height, g_ground.size(), natives::GetClockHours(), natives::GetClockMinutes(),
				g_playing ? "playing the guest" : "not playing");
	}

	void script_main()
	{
		log_line("script running");
		load_sun();
		load_settings();
		while (true)
		{
			tick();
			WAIT(0);
		}
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		g_module = module;
		open_outputs();
		scriptRegister(module, script_main);
		break;
	case DLL_PROCESS_DETACH:
		scriptUnregister(module);
		break;
	}
	return TRUE;
}
