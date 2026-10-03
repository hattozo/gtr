#include "Bridge.h"

#include "Engine.h"
#include "DataModel/Camera.h"
#include "DataModel/DataModel.h"
#include "DataModel/Explosion.h"
#include "DataModel/Folder.h"
#include "DataModel/Humanoid/Humanoid.h"
#include "Input/InputSystem.h"
#include "DataModel/Input/UserInputService.h"
#include "DataModel/Model.h"
#include "DataModel/PartInstance.h"
#include "DataModel/Player/Player.h"
#include "DataModel/Script/ScriptContext.h"
#include "DataModel/Service/Lighting.h"
#include "DataModel/Service/Players.h"
#include "DataModel/SpawnLocation.h"
#include "DataModel/Tool/Tool.h"
#include "DataModel/Value/StringValue.h"
#include "DataModel/Workspace.h"
#include "DataType/CFrame.h"
#include "DataType/Color3.h"
#include "Util/Log.h"
#include "Util/XmlSerializer.h"

#include <nlohmann/json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>

using namespace Vanadium;
using namespace Gtr;
using Json = nlohmann::json;

namespace {
constexpr int sProtocolVersion = 1;
constexpr const char *sCameraName = "HostCamera";
constexpr const char *sLightCameraName = "HostLightCamera";
// D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH, from d3dkmthk.h, which MinGW doesn't ship. REALTIME (5) needs a privilege
constexpr int sGpuPriorityHigh = 4;
// The shadow map is drawn from this far towards the sun (studs), far enough that its rays are as good as parallel, and takes
// in this far round the character
constexpr float sLightDistance = 700.0f;
constexpr float sLightReach = 22.0f;
constexpr const char *sBrickFolderName = "HostBricks";
constexpr const char *sGroundFolderName = "HostGround";
constexpr std::chrono::seconds sRateInterval(10);
constexpr std::chrono::milliseconds sInputTimeout(250);
// An explosion's shake: the largest swing of the camera for one at the camera, how many blast radii away it is still at full
// strength, the swing below which it isn't worth making, and how it dies away
constexpr float sShakeDegrees = 2.5f;
constexpr float sShakeReach = 3.0f;
constexpr float sShakeFaintest = 0.05f;
constexpr double sShakeDecaySeconds = 0.18;
constexpr std::chrono::milliseconds sShakeDuration(900);
// The host's own shakes, when they have been recorded from it (host/shake_record.py puts the file beside the guest), and the
// one of them played for an explosion. GTA has others by the names of small, medium and large explosions, which swing the
// camera through 10 to 18 degrees at full strength: those are for set pieces, not for a rocket.
constexpr float sLargestBlow = 150.0f;
constexpr double sFrameStatsSeconds = 10.0;
// Getting into one of the host's vehicles: how long the character takes to reach the seat, and how high it hops on the way (studs)
constexpr float sSeatSeconds = 0.5f;
constexpr float sSeatHop = 1.6f;
constexpr const char *sScriptFolder = "scripts";
constexpr const char *sPlaceScripts[] = { "Dress", "Trainer", "Rcl", "Pistol" };
constexpr const char *sRequestName = "HostRequest";
// What is drawn with a part can reach this far beyond it (studs: a sword's mesh, a rocket's flame), an explosion this many
// blast radii, and the part of the picture found to be drawn in is widened by this many pixels; nearer the camera than this
// (studs) a point has no place in the picture
constexpr float sDrawnPadding = 2.5f;
constexpr float sExplosionReach = 1.5f;
constexpr float sDrawnMargin = 24.0f;
constexpr float sNearestDrawn = 0.2f;
const Vector3 sSpareGroundPlace(0.0f, -20000.0f, 0.0f);
constexpr const char *sShakeFile = "GtrShakes.txt";
constexpr const char *sExplosionShake = "GRENADE_EXPLOSION_SHAKE";
// A blast as large as a rocket's shakes this much harder than a smaller one
constexpr float sLargeBlastMetres = 3.0f;
constexpr float sLargeBlastGain = 1.5f;
// A recorded shake is at full strength at the blast and dies away to nothing this many blast radii from it
constexpr float sShakeReachRadii = 9.0f;
constexpr const wchar_t *sWindowTitle = L"Vanadium (GTA passthrough guest)";
// The guest's own window only has to carry the place's camera and pointer, so it is kept small: its picture is drawn as
// well as the exported one
constexpr int sWindowWidth = 960;
// A classic character's root part is centred this many studs above its feet
constexpr float sRootHeight = 3.0f;
constexpr const char *sSpawnName = "HostSpawn";
const Vector3 sSpawnSize(8.0f, 1.0f, 8.0f);
// How far above the spawn point a character is put down, so it lands on its feet instead of starting inside the ground
constexpr float sSpawnDrop = 3.5f;

// Adds the time it lives for to a sum, in milliseconds
class Timed {
public:
    explicit Timed(double &sum) : mSum(sum), mStart(std::chrono::steady_clock::now()) {}
    ~Timed() { mSum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mStart).count(); }
private:
    double &mSum;
    std::chrono::steady_clock::time_point mStart;
};

template <typename T>
bool ReadList(const Json &message, const char *key, std::vector<T> &out) {
    const auto found = message.find(key);
    if (found == message.end() || !found->is_array())
        return false;
    out.reserve(found->size());
    for (const Json &item : *found) {
        if (!item.is_number())
            return false;
        out.push_back(item.get<T>());
    }
    return true;
}

// The cell a host point is in, as one number: its column and row in a grid of the given cell size
uint64_t CellKey(double x, double y, float cell) {
    const auto column = (int32_t)std::floor(x / cell);
    const auto row = (int32_t)std::floor(y / cell);
    return ((uint64_t)(uint32_t)column << 32) | (uint32_t)row;
}

template <typename T, size_t N>
bool ReadArray(const Json &message, const char *key, T (&out)[N]) {
    const auto found = message.find(key);
    if (found == message.end() || !found->is_array() || found->size() != N)
        return false;
    for (size_t i = 0; i < N; i++) {
        if (!(*found)[i].is_number())
            return false;
        out[i] = (*found)[i].get<T>();
    }
    return true;
}
}

Bridge::Bridge(Engine &engine, DataModel &dataModel, const BridgeOptions &options) :
    mEngine(engine),
    mDataModel(dataModel),
    mOptions(options),
    mStudsPerMetre(1.0f / options.MetresPerStud),
    mExporter(*engine.GetRenderer(), options.MetresPerStud),
    mHostBodies(dataModel, engine.GetInput(), mOrigin, options.MetresPerStud)
{}

bool Bridge::Open() {
    if (!mExporter.Open())
        return false;

    // The guest's window sits behind the host's, and the host is a game that keeps every core busy: Windows gives a process
    // in the background a small share of them and slows it further to save power, and every frame the guest is late is delay
    // the player feels. So the guest asks for a high priority and no throttling
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    PROCESS_POWER_THROTTLING_STATE throttling {};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
    // The GPU likewise goes to the window in front: with the host's being played the guest drew 12 to 33 frames a second, and
    // 88 as soon as the host lost the focus. With the GPU's own scheduler on (Windows 11's default) this may do little;
    // the compositor holding the host back for the guest's frames is what does the most (GtrHostState::FreeRunning)
    using SetGpuPriority = LONG(WINAPI *)(HANDLE, int);
    const HMODULE gdi = LoadLibraryA("gdi32.dll");
    const auto setGpuPriority = gdi != nullptr ? reinterpret_cast<SetGpuPriority>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass")) : nullptr;
    const LONG gpuStatus = setGpuPriority != nullptr ? setGpuPriority(GetCurrentProcess(), sGpuPriorityHigh) : -1;
    Log::Print("Gtr", "GPU priority {}", gpuStatus == 0 ? "high" : std::format("unchanged ({:#x})", (unsigned long)gpuStatus));
    if (!mLink.Listen(mOptions.Port)) {
        Log::Print("Gtr", "Could not listen on 127.0.0.1:{}", (int)mOptions.Port);
        return false;
    }

    auto *camera = mDataModel.GetWorkspace()->CreateChild<Camera>();
    camera->SetName(sCameraName);
    camera->SetCameraType(CameraType::Scriptable);
    mCamera = camera->GetHandle();
    auto *light = mDataModel.GetWorkspace()->CreateChild<Camera>();
    light->SetName(sLightCameraName);
    light->SetCameraType(CameraType::Scriptable);
    mLightCamera = light->GetHandle();

    // Something to stand on until the host says where its ground is; a place's character has already spawned over nothing
    PlaceSpawn(mOrigin, true);

    LoadShakes();
    // The host's picture is drawn in the exporter's own view, so the window's own, the same scene over again, is left out:
    // the window shows the interface alone
    mEngine.GetRenderer()->SetMainSceneDrawn(false);
    // Nor does anyone watch the window: it sits behind the host's, and a covered window that waits for vsync is held back by
    // the compositor, for 40 to 55 ms a frame while the host was being played. The guest paces itself instead (main.cpp)
    mEngine.GetRenderer()->Gpu.SetVSync(false);
    // The pointer is the place's own, drawn as part of the interface (the arrow, a tool's crosshair, UserInputService's
    // MouseIcon): the host shows that and no pointer of its own
    mDataModel.GetService<UserInputService>()->VNSetRobloxCursorEnabled(true);
    Log::Print("Gtr", "Waiting for the host on 127.0.0.1:{} ({} m per stud)", (int)mOptions.Port, mOptions.MetresPerStud);
    return true;
}

Vector3 Bridge::PointToGuest(const double host[3]) const {
    return Vector3((float)(host[0] - mOrigin[0]), (float)(host[2] - mOrigin[2]), (float)-(host[1] - mOrigin[1])) * mStudsPerMetre;
}

Vector3 Bridge::DirectionToGuest(const float host[3]) {
    return Vector3(host[0], host[2], -host[1]);
}

Instance *Bridge::GetFolder(std::shared_ptr<InstanceHandle> &handle, const char *name) {
    if (handle == nullptr || handle->Target == nullptr) {
        auto *folder = mDataModel.GetWorkspace()->CreateChild<Folder>();
        folder->SetName(name);
        handle = folder->GetHandle();
    }
    return handle->Target;
}

// The pad's top is the given point, so a character spawned on it stands with its feet there
void Bridge::PlaceSpawn(const double host[3], bool moveCharacter) {
    auto *pad = mSpawn != nullptr ? static_cast<SpawnLocation *>(mSpawn->Target) : nullptr;
    if (pad == nullptr) {
        pad = new SpawnLocation();
        pad->SetName(sSpawnName);
        pad->SetAnchored(true);
        pad->SetTransparency(1.0f);
        pad->SetNeutral(true);
        // No force field on the character it spawns
        pad->SetDuration(0);
        pad->SetSize(sSpawnSize);
        mSpawn = pad->GetHandle();
    }
    const Vector3 top = PointToGuest(host);
    pad->SetCFrame(CFrame(top - Vector3(0.0f, sSpawnSize.Y * 0.5f, 0.0f)));
    if (pad->GetParent() == nullptr)
        pad->SetParent(mDataModel.GetWorkspace());

    Player *player = mDataModel.GetService<Players>()->GetLocalPlayer();
    Model *character = player != nullptr ? player->GetCharacter() : nullptr;
    if (moveCharacter && character != nullptr)
        PlaceCharacter(top + Vector3(0.0f, sSpawnDrop, 0.0f));
}

// tiles holds x, y and the ground's height for each cell, in host space
void Bridge::SetGround(const std::vector<double> &tiles, float cell, float depth) {
    Instance *folder = GetFolder(mGroundFolder, sGroundFolderName);
    const Vector3 size = Vector3(cell, depth, cell) * mStudsPerMetre;
    for (size_t i = 0; i + 2 < tiles.size(); i += 3) {
        const uint64_t key = CellKey(tiles[i], tiles[i + 1], cell);
        std::shared_ptr<InstanceHandle> &handle = mGround[key];
        auto *part = handle != nullptr ? static_cast<PartInstance *>(handle->Target) : nullptr;
        // A tile the host dropped is taken up again before a new one is made: as the character walks, a row of tiles is dropped
        // behind it and a row is wanted ahead, and making and destroying parts by the hundred a second is what costs
        while (part == nullptr && !mSpareGround.empty()) {
            handle = mSpareGround.back();
            mSpareGround.pop_back();
            part = static_cast<PartInstance *>(handle->Target);
        }
        const bool created = part == nullptr;
        if (created) {
            part = new PartInstance();
            part->SetName("Ground");
            part->SetAnchored(true);
            part->SetLocked(true);
            part->SetTransparency(mGroundVisible ? 0.0f : 1.0f);
            for (const NormalId face : { NormalId::Top, NormalId::Bottom })
                part->SetSurface(face, SurfaceType::Smooth);
            handle = part->GetHandle();
        }
        if (created || mGroundVisible) {
            // A checkerboard, for when the ground is shown to check it against the host's
            const bool odd = ((int64_t)std::floor(tiles[i] / cell) + (int64_t)std::floor(tiles[i + 1] / cell)) & 1;
            part->SetColor(odd ? Color3(0.25f, 0.65f, 0.95f) : Color3(0.95f, 0.45f, 0.25f));
        }
        const double top[3] { tiles[i], tiles[i + 1], tiles[i + 2] };
        part->SetSize(size);
        part->SetCFrame(CFrame(PointToGuest(top) - Vector3(0.0f, size.Y * 0.5f, 0.0f)));
        if (created)
            part->SetParent(folder);
    }
}

void Bridge::DropGround(const std::vector<double> &cells, float cell) {
    for (size_t i = 0; i + 1 < cells.size(); i += 2) {
        const auto found = mGround.find(CellKey(cells[i], cells[i + 1], cell));
        if (found == mGround.end())
            continue;
        // Put out of the way, far below, to be a tile somewhere else
        if (auto *part = static_cast<PartInstance *>(found->second->Target)) {
            part->SetCFrame(CFrame(sSpareGroundPlace));
            mSpareGround.push_back(found->second);
        }
        mGround.erase(found);
    }
}

void Bridge::ShowGround(bool visible) {
    mGroundVisible = visible;
    for (const auto &[key, handle] : mGround) {
        if (auto *part = static_cast<PartInstance *>(handle->Target))
            part->SetTransparency(visible ? 0.0f : 1.0f);
    }
}

Model *Bridge::GetCharacter() const {
    Player *player = mDataModel.GetService<Players>()->GetLocalPlayer();
    return player != nullptr ? player->GetCharacter() : nullptr;
}

Humanoid *Bridge::GetHumanoid() const {
    Model *character = GetCharacter();
    return character != nullptr ? character->FindFirstChildWhichIsA<Humanoid>() : nullptr;
}

// The part the character stands and turns by: its Humanoid's root (HumanoidRootPart), not the model's PrimaryPart, which
// in a classic R6 model, Vanadium's own default among them, is the head
BasePart *Bridge::GetRoot() const {
    Humanoid *humanoid = GetHumanoid();
    if (BasePart *root = humanoid != nullptr ? humanoid->GetRootPart() : nullptr)
        return root;
    Model *character = GetCharacter();
    return character != nullptr ? character->GetPrimaryPart() : nullptr;
}

// Where the character's root is and which way it faces
CFrame Bridge::GetRootFrame() const {
    if (const BasePart *root = GetRoot())
        return root->GetCFrame();
    const Model *character = GetCharacter();
    return character != nullptr ? character->GetPivot() : CFrame();
}

// Moves the whole character so that its root is at a point, still, as it was turned
void Bridge::PlaceCharacter(const Vector3 &rootAt) {
    Model *character = GetCharacter();
    if (character == nullptr)
        return;
    CFrame pivot = character->GetPivot();
    pivot.translation = pivot.translation + (rootAt - GetRootFrame().translation);
    character->PivotTo(pivot);
    for (Instance *descendant : character->GetDescendants()) {
        if (auto *part = dynamic_cast<BasePart *>(descendant))
            part->SetAssemblyLinearVelocity(Vector3(0.0f, 0.0f, 0.0f));
    }
}

// Everything the guest draws is a part or goes with one (a mesh, a decal, a flame), or is an explosion: the boxes of those,
// seen from the camera, bound what the picture can show. The ground and the host's bodies, which are never drawn, are left
// out, and they are nearly all the parts there are.
PixelRect Bridge::FindDrawn(const Camera &camera, uint32_t width, uint32_t height) const {
    const PixelRect whole { 0, 0, width, height };
    if (mGroundVisible || mHostBodies.IsShown())
        return whole;
    const CFrame view = camera.GetCFrame();
    const float tanY = std::tan(camera.GetFieldOfView() * std::numbers::pi_v<float> / 360.0f);
    const float tanX = tanY * (float)width / (float)height;
    float low[2] { 1.0f, 1.0f }, high[2] { -1.0f, -1.0f };
    bool everywhere = false;

    const auto add = [&](const CFrame &frame, const Vector3 &half) {
        int behind = 0;
        float boxLow[2] { 1e9f, 1e9f }, boxHigh[2] { -1e9f, -1e9f };
        for (int corner = 0; corner < 8; corner++) {
            const Vector3 local((corner & 1) != 0 ? half.X : -half.X, (corner & 2) != 0 ? half.Y : -half.Y, (corner & 4) != 0 ? half.Z : -half.Z);
            const Vector3 seen = view.PointToObjectSpace(frame.PointToWorldSpace(local));
            // The camera looks down its -Z
            if (-seen.Z < sNearestDrawn) {
                behind++;
                continue;
            }
            const float at[2] { seen.X / (-seen.Z * tanX), seen.Y / (-seen.Z * tanY) };
            for (int axis = 0; axis < 2; axis++) {
                boxLow[axis] = std::min(boxLow[axis], at[axis]);
                boxHigh[axis] = std::max(boxHigh[axis], at[axis]);
            }
        }
        if (behind == 8)
            return;
        // A box the camera is inside of, or beside, can be anywhere in the picture
        if (behind > 0) {
            everywhere = true;
            return;
        }
        for (int axis = 0; axis < 2; axis++) {
            low[axis] = std::min(low[axis], boxLow[axis]);
            high[axis] = std::max(high[axis], boxHigh[axis]);
        }
    };

    const Instance *ground = mGroundFolder != nullptr ? mGroundFolder->Target : nullptr;
    const Instance *bodies = mHostBodies.GetFolderInstance();
    const auto visit = [&](const auto &self, const Instance *instance) -> void {
        if (instance == ground || instance == bodies || everywhere)
            return;
        if (const auto *part = dynamic_cast<const BasePart *>(instance)) {
            if (part->GetTransparency() < 1.0f)
                add(part->GetCFrame(), part->GetSize() * 0.5f + Vector3(sDrawnPadding, sDrawnPadding, sDrawnPadding));
        } else if (const auto *explosion = dynamic_cast<const Explosion *>(instance)) {
            const float reach = std::max(explosion->GetBlastRadius(), 4.0f) * sExplosionReach;
            add(CFrame(explosion->GetPosition()), Vector3(reach, reach, reach));
        }
        for (const Instance *child : instance->ViewChildren())
            self(self, child);
    };
    visit(visit, mDataModel.GetWorkspace());
    // What a tool has outlined is drawn too, though its part may not be: a body's box is not
    for (const DataModel::PartOutline &outline : mDataModel.GetPartOutlines()) {
        if (const auto *part = outline.Part != nullptr ? static_cast<const BasePart *>(outline.Part->Target) : nullptr)
            add(part->GetCFrame(), part->GetSize() * 0.5f + Vector3(sDrawnPadding, sDrawnPadding, sDrawnPadding));
    }
    if (everywhere)
        return whole;
    if (high[0] < low[0] || high[1] < low[1] || high[0] < -1.0f || low[0] > 1.0f || high[1] < -1.0f || low[1] > 1.0f)
        return { 0, 0, 16, 16 };

    // From the camera's -1..1, y up, to pixels, y down, and a margin round it
    const float left = (low[0] * 0.5f + 0.5f) * (float)width - sDrawnMargin, right = (high[0] * 0.5f + 0.5f) * (float)width + sDrawnMargin;
    const float top = (0.5f - high[1] * 0.5f) * (float)height - sDrawnMargin, bottom = (0.5f - low[1] * 0.5f) * (float)height + sDrawnMargin;
    PixelRect rect;
    rect.X = (uint32_t)std::clamp(left, 0.0f, (float)width - 16.0f);
    rect.Y = (uint32_t)std::clamp(top, 0.0f, (float)height - 16.0f);
    rect.Width = (uint32_t)std::clamp(right - (float)rect.X, 16.0f, (float)(width - rect.X));
    rect.Height = (uint32_t)std::clamp(bottom - (float)rect.Y, 16.0f, (float)(height - rect.Y));
    return rect;
}

// Holds the character in its seat. Getting in, it is moved there from where it stood in a short hop, turning to face the way
// the seat does: the host opens the vehicle's door meanwhile.
void Bridge::HoldSeat() {
    if (!mSeated)
        return;
    Model *character = GetCharacter();
    Humanoid *humanoid = GetHumanoid();
    BasePart *root = character != nullptr ? GetRoot() : nullptr;
    if (root == nullptr || humanoid == nullptr || humanoid->GetHealth() <= 0.0f) {
        // Dead, or gone: nothing to hold
        mSeated = false;
        if (root != nullptr)
            root->SetAnchored(false);
        return;
    }
    root->SetAnchored(true);
    humanoid->SetSit(true);
    const float t = std::clamp(std::chrono::duration<float>(std::chrono::steady_clock::now() - mSeatSince).count() / sSeatSeconds, 0.0f, 1.0f);
    if (t >= 1.0f) {
        root->SetCFrame(mSeat);
        return;
    }
    const float eased = t * t * (3.0f - 2.0f * t);
    CFrame frame = mSeatFrom.Lerp(mSeat, eased);
    frame.translation = frame.translation + Vector3(0.0f, std::sin(t * std::numbers::pi_v<float>) * sSeatHop, 0.0f);
    root->SetCFrame(frame);
}

// Out of the vehicle: the character stands again, where the host says the ground beside the vehicle is
void Bridge::LeaveSeat(const double host[3]) {
    if (!mSeated)
        return;
    mSeated = false;
    Model *character = GetCharacter();
    if (character == nullptr)
        return;
    if (BasePart *root = GetRoot())
        root->SetAnchored(false);
    if (Humanoid *humanoid = GetHumanoid())
        humanoid->SetSit(false);
    PlaceCharacter(PointToGuest(host) + Vector3(0.0f, sSpawnDrop, 0.0f));
    for (Instance *descendant : character->GetDescendants()) {
        if (auto *part = dynamic_cast<BasePart *>(descendant))
            part->SetAssemblyLinearVelocity(Vector3(0.0f, 0.0f, 0.0f));
    }
}

// What the guest adds to whatever place it plays is written as scripts, kept in a folder beside the guest and run as a line
// typed into Studio's command bar is: the character's looks and the building tools (Dress), and the trainer's menu and its
// zombies (Trainer). Done once there is a player to do it for; a tool can have one run again after changing it.
void Bridge::RunPlaceScript(const std::string &name) {
    std::ifstream file(std::string(sScriptFolder) + "/" + name + ".lua");
    if (!file) {
        Log::Print("Gtr", "No script {}/{}.lua", sScriptFolder, name);
        return;
    }
    std::stringstream source;
    source << file.rdbuf();
    mDataModel.GetService<ScriptContext>()->RunCommand(source.str(), [name](const std::optional<std::string> &error) {
        if (error.has_value())
            Log::Print("Gtr", "The script {} failed: {}", name, *error);
    });
}

void Bridge::DressPlace() {
    for (const char *name : sPlaceScripts)
        RunPlaceScript(name);
}

// A script of the place asks the host for something by leaving a StringValue named HostRequest, holding the message, under the
// host's camera: scripts have no other way out of the place
void Bridge::PassOnRequests() {
    Instance *camera = mCamera != nullptr ? mCamera->Target : nullptr;
    if (camera == nullptr)
        return;
    while (auto *request = dynamic_cast<StringValue *>(camera->FindFirstChild(sRequestName))) {
        if (mHostClient != 0)
            mLink.Send(mHostClient, request->GetValue());
        request->Destroy();
    }
}

// The guest's sun goes where its clock and its latitude put it, so those two are looked for: coarsely over the day and the
// globe, then closely round the best
void Bridge::FitSun(const Vector3 &towardsSun) {
    auto *lighting = mDataModel.GetService<Lighting>();
    const float clockBefore = lighting->GetClockTime(), latitudeBefore = lighting->GetGeographicLatitude();
    float bestClock = 14.0f, bestLatitude = latitudeBefore, best = -2.0f;
    const auto search = [&](float clockFrom, float clockTo, float clockStep, float latitudeFrom, float latitudeTo, float latitudeStep) {
        for (float clock = clockFrom; clock <= clockTo; clock += clockStep) {
            for (float latitude = latitudeFrom; latitude <= latitudeTo; latitude += latitudeStep) {
                lighting->SetClockTime(clock);
                lighting->SetGeographicLatitude(latitude);
                if (const float match = lighting->GetSunDirection().Dot(towardsSun); match > best) {
                    best = match;
                    bestClock = clock;
                    bestLatitude = latitude;
                }
            }
        }
    };
    search(6.5f, 17.5f, 0.5f, -85.0f, 85.0f, 10.0f);
    search(std::max(bestClock - 0.5f, 6.5f), std::min(bestClock + 0.5f, 17.5f), 0.05f, std::max(bestLatitude - 10.0f, -89.0f), std::min(bestLatitude + 10.0f, 89.0f), 1.0f);
    lighting->SetClockTime(clockBefore);
    lighting->SetGeographicLatitude(latitudeBefore);
    mHostClock = bestClock;
    mHostLatitude = bestLatitude;
    Log::Print("Gtr", "The host's sun: clock {:.2f}, latitude {:.0f}, {:.1f} degrees off", bestClock, bestLatitude,
               std::acos(std::clamp(best, -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>);
}

// A host that has stopped sending (paused, closed) can't say that a key came up, so what it held is let go for it
void Bridge::ReleaseStaleInput() {
    if ((mHeldKeys.empty() && mHeldButtons.empty()) || std::chrono::steady_clock::now() - mHostHeardAt < sInputTimeout)
        return;
    InputSystem *input = mEngine.GetInput();
    for (const int key : mHeldKeys)
        input->SimulateButton((KeyCode)key, false);
    for (const int button : mHeldButtons)
        input->SimulateMouseButton((UserInputType)button, false);
    mHeldKeys.clear();
    mHeldButtons.clear();
}

// The guest's window, where the place's own camera draws, its pointer is aimed and its interface is laid out, takes the size
// of the host's picture: a pointer the host puts a third of the way across is then on the same ray in both, and the interface,
// which the host shows over its picture, is drawn pixel for pixel
void Bridge::MatchWindow() {
    if (mWidth == 0 || mHeight == 0 || (mWidth == mWindowFor[0] && mHeight == mWindowFor[1]))
        return;
    mWindowFor[0] = mWidth;
    mWindowFor[1] = mHeight;
    const HWND window = FindWindowW(nullptr, sWindowTitle);
    DWORD process = 0;
    if (window == nullptr || GetWindowThreadProcessId(window, &process) == 0 || process != GetCurrentProcessId())
        return;
    RECT rect { 0, 0, (LONG)mWidth, (LONG)mHeight };
    AdjustWindowRect(&rect, (DWORD)GetWindowLongPtrW(window, GWL_STYLE), FALSE);
    SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// A camera pose in the host's terms. Guest (x, y, z) is host (x, -z, y).
HostPose Bridge::PoseToHost(const CFrame &frame, float fovY) const {
    const auto direction = [](const Vector3 &guest, float (&out)[3]) {
        out[0] = guest.X;
        out[1] = -guest.Z;
        out[2] = guest.Y;
    };
    HostPose pose;
    const Vector3 position = frame.translation * mOptions.MetresPerStud;
    pose.Position[0] = mOrigin[0] + position.X;
    pose.Position[1] = mOrigin[1] - position.Z;
    pose.Position[2] = mOrigin[2] + position.Y;
    direction(frame.RightVector(), pose.Right);
    direction(frame.LookVector(), pose.Forward);
    direction(frame.UpVector(), pose.Up);
    pose.FovY = fovY;
    return pose;
}

// The file is a line "shake NAME rate" for each shake, followed by its samples, six numbers a line
void Bridge::LoadShakes() {
    mShakeTraces.clear();
    std::ifstream file(sShakeFile);
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream words(line);
        if (line.starts_with("shake ")) {
            std::string word;
            ShakeTrace trace;
            words >> word >> trace.Name >> trace.Rate;
            if (trace.Rate > 0.0f)
                mShakeTraces.push_back(std::move(trace));
        } else if (!mShakeTraces.empty()) {
            std::array<float, 6> sample {};
            if (words >> sample[0] >> sample[1] >> sample[2] >> sample[3] >> sample[4] >> sample[5])
                mShakeTraces.back().Samples.push_back(sample);
        }
    }
    std::erase_if(mShakeTraces, [](const ShakeTrace &trace) { return trace.Samples.size() < 2; });
    Log::Print("Gtr", "{} of the host's camera shakes read from {}", mShakeTraces.size(), sShakeFile);
}

void Bridge::AddShake(float radius, float distance) {
    const auto now = std::chrono::steady_clock::now();
    radius = std::max(radius, 0.35f);
    if (const auto found = std::ranges::find(mShakeTraces, sExplosionShake, &ShakeTrace::Name); found != mShakeTraces.end()) {
        const float amplitude = (1.0f - distance / (radius * sShakeReachRadii)) * (radius >= sLargeBlastMetres ? sLargeBlastGain : 1.0f);
        if (amplitude > 0.0f)
            mShakes.push_back({ now, 0.0f, (int)(found - mShakeTraces.begin()), amplitude });
        return;
    }
    // Nothing recorded from the host: a shake made up here. Full strength within the blast, then falling off with distance
    // until it is too faint to bother with
    const float degrees = sShakeDegrees * std::min(1.0f, sShakeReach * radius / std::max(distance, radius));
    if (degrees > sShakeFaintest)
        mShakes.push_back({ now, degrees });
}

// How far the camera is knocked off its pose this frame, in its own space: the host's own shakes played back, or, without
// them, a few quick swings that die away
CFrame Bridge::GetShake() {
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(mShakes, [this, now](const Shake &shake) {
        const double seconds = std::chrono::duration<double>(now - shake.Start).count();
        if (shake.Trace < 0)
            return seconds > std::chrono::duration<double>(sShakeDuration).count();
        const ShakeTrace &trace = mShakeTraces[shake.Trace];
        return seconds * trace.Rate >= (double)(trace.Samples.size() - 1);
    });
    double pitch = 0.0, yaw = 0.0, roll = 0.0;
    Vector3 offset(0.0f, 0.0f, 0.0f);
    for (const Shake &shake : mShakes) {
        const double t = std::chrono::duration<double>(now - shake.Start).count();
        if (shake.Trace >= 0) {
            const ShakeTrace &trace = mShakeTraces[shake.Trace];
            const double at = t * trace.Rate;
            const auto index = (size_t)at;
            const auto blend = (float)(at - (double)index);
            std::array<float, 6> sample;
            for (size_t i = 0; i < sample.size(); i++)
                sample[i] = (trace.Samples[index][i] + (trace.Samples[index + 1][i] - trace.Samples[index][i]) * blend) * shake.Amplitude;
            // The host's camera has its forward where this one has its back, and rolls about it the other way
            offset = offset + Vector3(sample[0], sample[2], -sample[1]) * mStudsPerMetre;
            pitch += sample[3] * (std::numbers::pi / 180.0);
            roll -= sample[4] * (std::numbers::pi / 180.0);
            yaw += sample[5] * (std::numbers::pi / 180.0);
            continue;
        }
        const double strength = shake.Degrees * (std::numbers::pi / 180.0) * std::exp(-t / sShakeDecaySeconds);
        pitch += strength * std::sin(2.0 * std::numbers::pi * 17.0 * t);
        yaw += strength * 0.7 * std::sin(2.0 * std::numbers::pi * 23.0 * t + 1.3);
        roll += strength * 0.5 * std::sin(2.0 * std::numbers::pi * 13.0 * t + 2.1);
    }
    return CFrame(offset) * CFrame::Angles(pitch, yaw, roll);
}

// What explodes in the guest explodes in the host: each new Explosion in the workspace is told to the host once. It shakes the
// camera too, the more the nearer it is.
void Bridge::ReportExplosions() {
    std::erase_if(mExplosions, [](const std::shared_ptr<InstanceHandle> &handle) { return handle->Target == nullptr; });
    if (mHostClient == 0 || !mHasOrigin)
        return;
    const Camera *own = mDataModel.GetWorkspace()->FindCurrentCamera();
    for (Instance *child : mDataModel.GetWorkspace()->ViewChildren()) {
        const auto *explosion = dynamic_cast<Explosion *>(child);
        if (explosion == nullptr || std::ranges::find(mExplosions, child->GetHandle()) != mExplosions.end())
            continue;
        mExplosions.push_back(child->GetHandle());
        if (own != nullptr)
            AddShake(explosion->GetBlastRadius() * mOptions.MetresPerStud,
                     (own->GetCFrame().translation - explosion->GetPosition()).Magnitude() * mOptions.MetresPerStud);
        const Vector3 position = explosion->GetPosition() * mOptions.MetresPerStud;
        mLink.Send(mHostClient, std::format("{{\"t\":\"explosion\",\"pos\":[{:.3f},{:.3f},{:.3f}],\"radius\":{:.2f},\"pressure\":{:.0f}}}",
                                            mOrigin[0] + position.X, mOrigin[1] - position.Z, mOrigin[2] + position.Y,
                                            explosion->GetBlastRadius() * mOptions.MetresPerStud, explosion->GetBlastPressure()));
    }
}

// Tells the host where the character and the place's camera are, in the host's terms: the character's feet and its heading,
// and the camera's position and rotation the way GTA writes one (pitch, roll, yaw; a heading of 0 faces +Y and turns
// counter-clockwise). Guest (x, y, z) is host (x, -z, y).
void Bridge::ReportState() {
    if (mHostClient == 0 || !mHasOrigin)
        return;
    constexpr double degrees = 180.0 / std::numbers::pi;
    const float scale = mOptions.MetresPerStud;

    std::string character = "null";
    if (Model *model = GetCharacter(); model != nullptr) {
        const Humanoid *humanoid = GetHumanoid();
        const CFrame pivot = GetRootFrame();
        const Vector3 feet = (pivot.translation - Vector3(0.0f, sRootHeight, 0.0f)) * scale;
        const Vector3 look = pivot.LookVector();
        character = std::format("[{:.4f},{:.4f},{:.4f},{:.2f},{:.1f}]", mOrigin[0] + feet.X, mOrigin[1] - feet.Z, mOrigin[2] + feet.Y,
                                std::atan2(-look.X, -look.Z) * degrees, humanoid != nullptr ? humanoid->GetHealth() : 0.0f);
    }

    std::string view = "null";
    Camera *camera = mDataModel.GetWorkspace()->FindCurrentCamera();
    if (camera != nullptr && (mCamera == nullptr || camera != mCamera->Target)) {
        const CFrame frame = camera->GetCFrame();
        const Vector3 position = frame.translation * scale;
        const Vector3 look = frame.LookVector();
        view = std::format("[{:.4f},{:.4f},{:.4f},{:.3f},0,{:.3f},{:.2f}]", mOrigin[0] + position.X, mOrigin[1] - position.Z, mOrigin[2] + position.Y,
                           std::asin(std::clamp(look.Y, -1.0f, 1.0f)) * degrees, std::atan2(-look.X, -look.Z) * degrees, camera->GetFieldOfView());
    }
    // Whether the place holds the pointer still to turn the camera (Enum.MouseBehavior): the host then has no cursor to show
    // And what the character has in its hand, by the tool's name, so the host's people can see it is armed
    std::string held;
    if (Model *model = GetCharacter(); model != nullptr) {
        if (const Instance *tool = model->FindFirstChildWhichIsA<Tool>())
            held = tool->GetName();
    }
    std::erase_if(held, [](char c) { return c == '"' || c == '\\' || (unsigned char)c < 32; });
    mLink.Send(mHostClient, std::format("{{\"t\":\"state\",\"tool\":\"{}\",\"char\":{},\"cam\":{},\"lock\":{}}}", held, character, view,
                                        (int)mEngine.GetInput()->GetMouseBehavior()), true);
}

void Bridge::Handle(int client, const std::string &line) {
    const Json message = Json::parse(line, nullptr, false);
    if (!message.is_object() || !message.contains("t") || !message["t"].is_string()) {
        Log::Print("Gtr", "Ignored a line from the host that is not a message: {}", line.substr(0, 120));
        return;
    }
    const std::string type = message["t"].get<std::string>();

    if (type == "origin") {
        // A new origin moves everything the host placed, so the host sends it before anything else and places it all again
        mHostClient = client;
        mHasOrigin = ReadArray(message, "pos", mOrigin);
        if (mHasOrigin)
            PlaceSpawn(mOrigin, false);
    } else if (type == "spawn") {
        double position[3];
        if (mHasOrigin && ReadArray(message, "pos", position))
            PlaceSpawn(position, message.value("move", true));
    } else if (type == "ground") {
        const float cell = message.value("cell", 1.0f);
        std::vector<double> tiles, dropped;
        if (!mHasOrigin || cell <= 0.0f)
            return;
        if (ReadList(message, "drop", dropped))
            DropGround(dropped, cell);
        if (ReadList(message, "tiles", tiles))
            SetGround(tiles, cell, message.value("depth", 3.0f));
    } else if (type == "light") {
        // Where the host's sun is, which the guest's is then put, so that what the guest draws is lit from the same side
        // as the host's world around it; or, with no sun to match, a time of day in hours
        float sun[3];
        if (ReadArray(message, "sun", sun)) {
            mSunDirection = DirectionToGuest(sun);
            FitSun(*mSunDirection);
        } else {
            mHostClock = message.value("clock", 14.0f);
            mHostLatitude.reset();
            mSunDirection.reset();
        }
    } else if (type == "perf") {
        // For measuring what the guest's frames cost the host while it is played: the guest's frame rate cap, and its
        // render quality level (1 to 21, Roblox's levels; 0 lets the renderer choose by frame time, as it does at first)
        if (message.contains("fps") && mSetFrameRate)
            mSetFrameRate(message.value("fps", 0));
        if (message.contains("quality")) {
            mEngine.GetRenderer()->SetForcedQualityLevel(message.value("quality", 0));
            Log::Print("Gtr", "Render quality {}", message.value("quality", 0) > 0 ? std::to_string(message.value("quality", 0)) : std::string("automatic"));
        }
    } else if (type == "debug") {
        if (message.contains("ground"))
            ShowGround(message.value("ground", false));
        if (message.contains("bodies"))
            mHostBodies.Show(message.value("bodies", false));
    } else if (type == "bodies") {
        if (mHasOrigin)
            mHostBodies.Handle(message);
    } else if (type == "walls") {
        if (mHasOrigin)
            mHostBodies.HandleWalls(message);
    } else if (type == "harm") {
        // Something of the host's has hurt the character: one of its people, or a vehicle
        // More than any one blow is a host that has lost count of its player's health, not a blow
        const float damage = message.value("damage", 0.0f);
        if (Humanoid *humanoid = GetHumanoid(); humanoid != nullptr && damage > 0.0f && damage <= sLargestBlow)
            humanoid->TakeDamage(damage);
    } else if (type == "cam") {
        HostPose pose;
        if (!ReadArray(message, "pos", pose.Position) || !ReadArray(message, "right", pose.Right) ||
            !ReadArray(message, "fwd", pose.Forward) || !ReadArray(message, "up", pose.Up))
            return;
        pose.CameraId = message.value("id", (int64_t)0);
        pose.FovY = message.value("fov", pose.FovY);
        mWidth = message.value("w", mWidth);
        mHeight = message.value("h", mHeight);
        if (!mHasOrigin) {
            std::copy(std::begin(pose.Position), std::end(pose.Position), std::begin(mOrigin));
            mHasOrigin = true;
        }
        mPose = pose;
        mHasPose = mWidth > 0 && mHeight > 0;
        mOwnCamera = false;
        // Whoever sends the camera is the host; the other clients are tools
        mHostClient = client;
    } else if (type == "view") {
        // The host's picture size, from a host that follows the place's own camera instead of sending one
        mWidth = message.value("w", mWidth);
        mHeight = message.value("h", mHeight);
        mOwnCamera = true;
        mHostClient = client;
    } else if (type == "shake") {
        // Something the host knows of shook the ground: a blast of this radius, this far from the camera, both in metres. Or
        // the host's shakes have been recorded anew
        if (message.value("reload", false))
            LoadShakes();
        else
            AddShake(message.value("radius", 4.0f), message.value("distance", 0.0f));
    } else if (type == "brick" && message.value("remove", false)) {
        if (const auto found = mBricks.find(message.value("id", std::string("brick"))); found != mBricks.end()) {
            if (found->second->Target != nullptr)
                found->second->Target->Destroy();
            mBricks.erase(found);
        }
    } else if (type == "brick") {
        double position[3];
        float size[3];
        if (!mHasOrigin || !ReadArray(message, "pos", position) || !ReadArray(message, "size", size))
            return;
        float color[3] { 0.64f, 0.64f, 0.64f };
        ReadArray(message, "color", color);
        const std::string id = message.value("id", std::string("brick"));

        std::shared_ptr<InstanceHandle> &handle = mBricks[id];
        auto *part = handle != nullptr ? static_cast<PartInstance *>(handle->Target) : nullptr;
        if (part == nullptr) {
            part = new PartInstance();
            part->SetName(id);
            part->SetAnchored(true);
            handle = part->GetHandle();
        }
        // The host gives the size along its own axes, so its depth (Y) and height (Z) trade places
        part->SetSize(Vector3(size[0], size[2], size[1]) * mStudsPerMetre);
        const double yaw = message.value("yaw", 0.0) * std::numbers::pi / 180.0;
        CFrame frame = CFrame::Angles(0.0, yaw, 0.0);
        frame.translation = PointToGuest(position);
        part->SetCFrame(frame);
        part->SetColor(Color3(color[0], color[1], color[2]));
        if (part->GetParent() == nullptr)
            part->SetParent(GetFolder(mBrickFolder, sBrickFolderName));
    } else if (type == "clear") {
        for (const auto &[id, handle] : mBricks) {
            if (handle->Target != nullptr)
                handle->Target->Destroy();
        }
        mBricks.clear();
        for (const auto &[key, handle] : mGround) {
            if (handle->Target != nullptr)
                handle->Target->Destroy();
        }
        mGround.clear();
        for (const auto &handle : mSpareGround) {
            if (handle->Target != nullptr)
                handle->Target->Destroy();
        }
        mSpareGround.clear();
        mHostBodies.Clear();
    } else if (type == "where") {
        // Where the local character stands, so a host can find it before it drives it
        Json reply { { "t", "where" }, { "character", nullptr } };
        if (Model *character = GetCharacter(); character != nullptr && mHasOrigin) {
            const Vector3 guest = GetRootFrame().translation * mOptions.MetresPerStud;
            reply["character"] = { mOrigin[0] + guest.X, mOrigin[1] - guest.Z, mOrigin[2] + guest.Y };
        }
        mLink.Send(client, reply.dump());
    } else if (type == "lua") {
        // Runs like a line typed into Studio's command bar. What the code returns comes back as a string, by way of a StringValue the
        // wrapped code fills, since a command hands back nothing but its error
        auto *camera = mCamera != nullptr ? mCamera->Target : nullptr;
        if (camera == nullptr)
            return;
        const int64_t id = message.value("id", (int64_t)0);
        const std::string replyName = "Reply" + std::to_string(id);
        auto *reply = camera->CreateChild<StringValue>();
        reply->SetName(replyName);
        const std::string source = std::format("local __reply = workspace.{}:FindFirstChild(\"{}\"); local __result = (function() {}\nend)(); "
                                               "if __reply then __reply.Value = tostring(__result) end",
                                               sCameraName, replyName, message.value("src", std::string()));
        mDataModel.GetService<ScriptContext>()->RunCommand(source, [this, id, client, handle = reply->GetHandle()](const std::optional<std::string> &error) {
            Json answer { { "t", "lua" }, { "id", id }, { "error", nullptr }, { "result", nullptr } };
            if (error.has_value())
                answer["error"] = *error;
            if (auto *value = static_cast<StringValue *>(handle->Target)) {
                if (!error.has_value())
                    answer["result"] = value->GetValue();
                value->Destroy();
            }
            mLink.Send(client, answer.dump());
        });
    } else if (type == "key") {
        // The host's window has the keyboard and mouse, and passes them on as they are: the place's own control, camera and
        // tool scripts then behave as they do in the place. The code is a Roblox KeyCode.
        const int code = message.value("code", 0);
        const bool down = message.value("down", false);
        mEngine.GetInput()->SimulateButton((KeyCode)code, down);
        if (down)
            mHeldKeys.insert(code);
        else
            mHeldKeys.erase(code);
    } else if (type == "mouse") {
        // The pointer as a fraction of the host's picture, and how far the mouse moved, which is what turns the camera
        float position[2], delta[2] { 0.0f, 0.0f };
        if (!ReadArray(message, "pos", position))
            return;
        ReadArray(message, "delta", delta);
        const Gfx::GpuContext &gpu = mEngine.GetRenderer()->Gpu;
        mEngine.GetInput()->SimulateMouseMove(Vector2(position[0] * (float)gpu.Width, position[1] * (float)gpu.Height), Vector2(delta[0], delta[1]));
    } else if (type == "button") {
        const int button = std::clamp(message.value("button", 0), 0, 2);
        const bool down = message.value("down", false);
        mEngine.GetInput()->SimulateMouseButton((UserInputType)button, down);
        if (down)
            mHeldButtons.insert(button);
        else
            mHeldButtons.erase(button);
    } else if (type == "wheel") {
        mEngine.GetInput()->SimulateMouseWheel(message.value("delta", 0.0f));
    } else if (type == "seat") {
        double position[3];
        float forward[3], up[3];
        if (ReadArray(message, "out", position)) {
            LeaveSeat(position);
        } else if (mHasOrigin && ReadArray(message, "pos", position) && ReadArray(message, "fwd", forward) && ReadArray(message, "up", up)) {
            const Vector3 ahead = DirectionToGuest(forward), above = DirectionToGuest(up);
            const Vector3 back(-ahead.X, -ahead.Y, -ahead.Z);
            mSeat = CFrame::FromMatrix(PointToGuest(position), above.Cross(back), above, back);
            if (!mSeated) {
                Model *character = GetCharacter();
                const BasePart *root = character != nullptr ? GetRoot() : nullptr;
                mSeated = root != nullptr;
                if (mSeated) {
                    mSeatFrom = root->GetCFrame();
                    mSeatSince = std::chrono::steady_clock::now();
                }
            }
        }
    } else if (type == "script") {
        RunPlaceScript(message.value("name", std::string()));
    } else if (type == "host") {
        // For the host's script, from a tool directing the session: passed on to everyone else
        mLink.Broadcast(line, client);
    } else if (type == "ping") {
        mLink.Send(client, Json({ { "t", "pong" }, { "id", message.value("id", 0) }, { "published", mExporter.GetPublishedCount() } }).dump());
    } else if (type == "save") {
        // From a tool: one of the place's instances, a service by its name or a child of one ("StarterPack",
        // "StarterPack.Sword"), written as Roblox XML to a file, in the way Vanadium saves a place. A service written alone
        // is a place with that service in it (see tools/make-tools-place.py)
        Instance *found = &mDataModel;
        std::stringstream names(message.value("what", std::string()));
        for (std::string name; found != nullptr && std::getline(names, name, '.');)
            found = found->FindFirstChild(name);
        const std::string path = message.value("path", std::string());
        const bool saved = found != nullptr && found != &mDataModel && !path.empty() && XmlSerializer(path).Write(found);
        mLink.Send(client, Json({ { "t", "saved" }, { "ok", saved }, { "path", path } }).dump());
    } else if (type == "quit") {
        mEngine.Shutdown();
    } else {
        Log::Print("Gtr", "Ignored an unknown \"{}\" message from the host", type);
    }
}

// What the host and the tools have sent since the last frame. Run at the start of a frame: a click is something the interface
// looks for as it is laid out, and what was pressed is forgotten at the end of the frame it was pressed in, so a click taken
// at the end of a frame was never seen. Starting here also has the place's scripts and camera act on it a frame sooner.
void Bridge::Receive() {
    // Hello first, so it is the first line a client reads even when its first message is answered this tick
    mCostTicks++;
    std::vector<HostLink::Received> received;
    {
        Timed timed(mCosts["poll"]);
        received = mLink.Poll();
    }
    for (const int client : mLink.TakeConnected()) {
        Log::Print("Gtr", "Client {} connected", client);
        mLink.Send(client, Json({ { "t", "hello" }, { "version", sProtocolVersion }, { "metresPerStud", mOptions.MetresPerStud } }).dump());
    }
    for (const HostLink::Received &line : received) {
        if (line.Client == mHostClient)
            mHostHeardAt = std::chrono::steady_clock::now();
        // By the kind of message, which is near the line's start or its end as the sender wrote it
        const size_t at = line.Line.find("\"t\":\"");
        const size_t end = at == std::string::npos ? at : line.Line.find('"', at + 5);
        Timed timed(mCosts[end == std::string::npos ? std::string("?") : line.Line.substr(at + 5, end - at - 5)]);
        Handle(line.Client, line.Line);
    }
    ReleaseStaleInput();
    HoldSeat();
}

void Bridge::Tick() {
    MatchWindow();
    if (!mPlaceDressed && mDataModel.GetService<Players>()->GetLocalPlayer() != nullptr) {
        mPlaceDressed = true;
        DressPlace();
        // Where each frame's time goes, in the log every so often: the guest's speed is the host's latency. Asked for here
        // and not at the start, where the engine sets its own interval afterwards
        mEngine.GetTaskScheduler()->SetFrameStatsInterval(sFrameStatsSeconds);
    }

    {
        Timed timed(mCosts["publish"]);
        mExporter.Publish();
    }
    {
        Timed timed(mCosts["report"]);
        PassOnRequests();
        ReportState();
        ReportExplosions();
        for (const std::string &line : mHostBodies.Tick(mGroundFolder != nullptr ? mGroundFolder->Target : nullptr)) {
            if (mHostClient != 0)
                mLink.Send(mHostClient, line);
        }
    }

    // How fast frames reach the host, since the guest's window is usually behind the host's and may be slowed for it
    const auto now = std::chrono::steady_clock::now();
    if (mLink.IsConnected() && now - mRateSince >= sRateInterval) {
        const int64_t published = mExporter.GetPublishedCount();
        if (mRateSince != std::chrono::steady_clock::time_point())
            Log::Print("Gtr", "{:.1f} frames a second exported at {}x{}; {} ground tiles, {} bricks, {} of the host's bodies",
                       (double)(published - mRatePublished) / std::chrono::duration<double>(now - mRateSince).count(), mWidth, mHeight,
                       mGround.size(), mBricks.size(), mHostBodies.GetCount());
        // Where the bridge's own time went, in milliseconds a frame
        std::string costs;
        for (auto &[name, sum] : mCosts) {
            if (sum / (double)std::max(mCostTicks, 1) >= 0.05)
                costs += std::format(" {} {:.2f}", name, sum / (double)std::max(mCostTicks, 1));
            sum = 0.0;
        }
        Log::Print("Gtr", "Bridge costs over {} frames:{}", mCostTicks, costs);
        mCostTicks = 0;
        mRateSince = now;
        mRatePublished = published;
    }
}

// The host's picture of this frame. It has to show the step the place's camera was placed for: the camera is placed by
// RenderStepped at the start of a frame, and the physics step at the end of it moves the character on. Drawn after that step,
// the character is a step ahead of the camera in every frame, and slides over the host's ground whenever it moves.
void Bridge::Draw() {
    // The renderer makes ready what a view draws with as it draws its own first frame
    if (!mRendererDrew) {
        mRendererDrew = true;
        return;
    }
    // Frames that came back from the GPU since the last tick go out now instead of at the end of this one
    {
        Timed timed(mCosts["publish"]);
        mExporter.Publish();
    }

    auto *camera = mCamera != nullptr ? static_cast<Camera *>(mCamera->Target) : nullptr;
    Camera *own = mDataModel.GetWorkspace()->FindCurrentCamera();
    // A camera learns how large its picture is from the view that draws it, and the window's view, which drew the place's
    // camera, is switched off: without this the pointer's rays, which the tools aim along, are worked out for no picture at all
    if (own != nullptr) {
        const Gfx::GpuContext &gpu = mEngine.GetRenderer()->Gpu;
        own->SetViewportSize(Vector2((float)gpu.Width, (float)gpu.Height));
    }
    bool draws = false;
    if (camera != nullptr && mOwnCamera && own != nullptr && own != camera && mHasOrigin) {
        // The place's camera is in charge: the picture is drawn from where it is this frame, shaken by what is exploding, and
        // carries that pose with it so the host can put its own camera in the same place for the same frame
        const CFrame frame = own->GetCFrame() * GetShake();
        camera->SetCFrame(frame);
        camera->SetFieldOfView(own->GetFieldOfView());
        mPose = PoseToHost(frame, own->GetFieldOfView());
        mPose.CameraId = ++mOwnFrames;
        draws = mWidth > 0 && mHeight > 0;
    } else if (camera != nullptr && !mOwnCamera && mHasPose) {
        // A CFrame looks down its -Z, so its Z axis is the camera's backward
        const Vector3 forward = DirectionToGuest(mPose.Forward);
        camera->SetCFrame(CFrame::FromMatrix(PointToGuest(mPose.Position), DirectionToGuest(mPose.Right), DirectionToGuest(mPose.Up),
                                             Vector3(-forward.X, -forward.Y, -forward.Z)));
        camera->SetFieldOfView(mPose.FovY);
        draws = true;
    }
    if (draws) {
        // Set here, each frame, just before the host's picture is drawn: a place with a day and night cycle of its own sets
        // the clock every frame too, and for what the host sees the host's time is the one that counts
        if (mHostClock.has_value())
            mDataModel.GetService<Lighting>()->SetClockTime(*mHostClock);
        if (mHostLatitude.has_value())
            mDataModel.GetService<Lighting>()->SetGeographicLatitude(*mHostLatitude);
        // The guest as the host's sun sees it, for its shadows on the host's world: from far off towards the sun, looking at
        // the character
        auto *light = mLightCamera != nullptr ? static_cast<Camera *>(mLightCamera->Target) : nullptr;
        const Model *character = GetCharacter();
        HostPose lightPose;
        if (light != nullptr && mSunDirection.has_value()) {
            // Without a character, what the camera looks at
            const Vector3 middle = character != nullptr ? GetRootFrame().translation
                                                        : camera->GetCFrame().translation + camera->GetCFrame().LookVector() * sLightReach;
            const Vector3 back = *mSunDirection * (1.0f / mSunDirection->Magnitude());
            const Vector3 aside = std::abs(back.Y) > 0.99f ? Vector3(1.0f, 0.0f, 0.0f) : Vector3(0.0f, 1.0f, 0.0f).Cross(back);
            const Vector3 right = aside * (1.0f / aside.Magnitude());
            // Moved a whole texel of the map at a time: a map that slides under what it holds makes the shadow's edge crawl
            const Vector3 above = back.Cross(right);
            const float texel = 2.0f * sLightReach / (float)GTR_LIGHT_SIZE;
            const Vector3 steady = middle - right * std::fmod(middle.Dot(right), texel) - above * std::fmod(middle.Dot(above), texel);
            const CFrame frame = CFrame::FromMatrix(steady + back * sLightDistance, right, above, back);
            const float fov = 2.0f * std::atan(sLightReach / sLightDistance) * 180.0f / std::numbers::pi_v<float>;
            light->SetCFrame(frame);
            light->SetFieldOfView(fov);
            lightPose = PoseToHost(frame, fov);
        } else {
            light = nullptr;
        }
        Timed timed(mCosts["render"]);
        mExporter.Render(camera, mWidth, mHeight, mPose, FindDrawn(*camera, mWidth, mHeight), light, lightPose);
    }
}
