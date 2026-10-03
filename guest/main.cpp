// The guest half of the passthrough: Vanadium, drawn from the host game's camera and exported for the host to composite into its
// own frame. With no place it starts an empty world for the host to fill. With a place it plays that place solo, by default without
// the place's map: the host's world is the map, so only what the place gives its players (tools, scripts, GUIs) is kept.
#include "Bridge.h"

#include "VanadiumSDLApp.h"
#include "Engine.h"
#include "DataModel/Camera.h"
#include "DataModel/DataModel.h"
#include "DataModel/PostEffect.h"
#include "DataModel/Service/Lighting.h"
#include "DataModel/Service/Players.h"
#include "DataModel/Service/RunService.h"
#include "DataModel/Workspace.h"
#include "DataType/Content.h"
#include "Util/BaseSerializer.h"
#include "Util/Log.h"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
// The host's picture is drawn just before the renderer draws the guest's own (9995): after the frame's scripts have placed
// the camera, before the physics step (9996) moves the character on from where that camera looks. The host is heard and
// answered once the frame is done.
// The wait for the next frame comes before the host is heard, so that what it says is as new as it can be when the frame acts
constexpr int sPaceJobPriority = -1;
// The host is heard before anything else in a frame (the renderer's own first job is 1), so that the frame acts on it
constexpr int sReceiveJobPriority = 0;
constexpr int sDrawJobPriority = 9994;
constexpr int sBridgeJobPriority = 9999;
constexpr const char *sDataModelName = "Passthrough";
// Mid-afternoon, until the host's own time of day drives the guest's lighting
constexpr float sDefaultClockTime = 14.0f;
// Frames a second when the display's own rate can't be read; Windows reports 0 or 1 for "the hardware's default"
constexpr int sFallbackFrameRate = 60;
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

struct GuestOptions {
    std::string Place;
    bool KeepMap { false };
    // Frames a second the guest keeps to; 0 for the display's refresh rate
    int FrameRate { 0 };
    Gtr::BridgeOptions Bridge;
};

// Takes the place's own world out, and the look it gave that world: the host supplies both
void RemoveMap(Vanadium::DataModel &dm) {
    Vanadium::Workspace *workspace = dm.GetWorkspace();
    for (Vanadium::Instance *child : workspace->GetChildren()) {
        if (dynamic_cast<Vanadium::Camera *>(child) == nullptr)
            child->Destroy();
    }
    auto *lighting = dm.GetService<Vanadium::Lighting>();
    for (Vanadium::Instance *child : lighting->GetChildren()) {
        if (dynamic_cast<Vanadium::PostEffect *>(child) != nullptr)
            child->Destroy();
    }
    lighting->SetClockTime(sDefaultClockTime);
}

// Holds the guest to a frame rate of its own, as vsync held it before it was turned off for the covered window, and while the
// host is ticking, to one frame for each of the host's. Measured with GTA in front: the guest drew 90 to 110 frames a second
// for GTA's 40, two in three of them for nothing, and GTA, sharing the GPU, was slower for all of them; capped lower, the
// compositor holds GTA to the guest's rate instead (it went to 30 with the guest at 30). Drawing as GTA ticks, neither waits on
// work the other throws away.
class FramePacer {
    using Clock = std::chrono::steady_clock;
public:
    explicit FramePacer(int frameRate) :
        mTimer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS))
    {
        SetRate(frameRate);
    }

    void SetRate(int frameRate) {
        mInterval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / frameRate));
        Vanadium::Log::Print("Gtr", "Paced to {} frames a second", frameRate);
    }

    void Wait() {
        const Clock::time_point now = Clock::now();
        if (now >= mNext) {
            // Behind: the next frame is timed from now, with no hurry to make up the frames that were late
            mNext = now + mInterval;
            return;
        }
        SleepFor(mNext - now);
        mNext += mInterval;
    }

    // Waits until the host has ticked since the last frame, as long as it is ticking at all
    void FollowHost() {
        const Clock::time_point start = Clock::now();
        if (mHost == nullptr && start >= mNextOpen) {
            mNextOpen = start + sHostRetry;
            if (HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, GTR_HOST_MAPPING_NAME)) {
                mHost = static_cast<const GtrHostState *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(GtrHostState)));
                CloseHandle(mapping);
            }
        }
        if (mHost == nullptr || mHost->Magic != GTR_HOST_MAGIC)
            return;
        // The host writes it while it is read
        const auto heartbeat = [this]() { return *reinterpret_cast<const volatile uint32_t *>(&mHost->Heartbeat); };
        uint32_t now = heartbeat();
        if (now != mHeartbeat) {
            mHeartbeat = now;
            mHeartbeatAt = start;
            return;
        }
        // A host whose script has stopped (its pause menu, or closed) isn't waited for
        if (start - mHeartbeatAt > sHostStalled)
            return;
        while ((now = heartbeat()) == mHeartbeat && Clock::now() - start < sHostWaitLongest)
            SleepFor(sHostPoll);
        if (now != mHeartbeat) {
            mHeartbeat = now;
            mHeartbeatAt = Clock::now();
        }
    }
private:
    void SleepFor(Clock::duration wait) {
        // In 100 ns units, negative for a time from now
        LARGE_INTEGER due;
        due.QuadPart = -std::chrono::duration_cast<std::chrono::duration<long long, std::ratio<1, 10000000>>>(wait).count();
        if (mTimer != nullptr && SetWaitableTimerEx(mTimer, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(mTimer, INFINITE);
        else
            Sleep((DWORD)std::chrono::duration_cast<std::chrono::milliseconds>(wait).count());
    }

    // How often the host's state is looked for until it is there; how long a host that hasn't ticked counts as stopped; the
    // longest a frame waits for the host's next tick (a host slower than 30 a second doesn't slow the guest further), and how
    // often it looks meanwhile
    static constexpr Clock::duration sHostRetry = std::chrono::seconds(1);
    static constexpr Clock::duration sHostStalled = std::chrono::milliseconds(200);
    static constexpr Clock::duration sHostWaitLongest = std::chrono::microseconds(33333);
    static constexpr Clock::duration sHostPoll = std::chrono::microseconds(500);

    Clock::duration mInterval;
    Clock::time_point mNext;
    HANDLE mTimer;
    const GtrHostState *mHost { nullptr };
    Clock::time_point mNextOpen;
    uint32_t mHeartbeat { 0 };
    Clock::time_point mHeartbeatAt;
};

int DisplayFrameRate() {
    DEVMODEW mode {};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        return (int)mode.dmDisplayFrequency;
    return sFallbackFrameRate;
}

class GuestApp : public Vanadium::SDLApp {
public:
    GuestApp(SDLApp::Init init, GuestOptions options) : SDLApp(std::move(init)), mOptions(std::move(options)) {}
protected:
    void ConfigureEngine(Vanadium::Init &init) override {
        init.PlaceFiles.clear();
    }

    void OnEngineLoad(Vanadium::Engine &engine) override {
        Vanadium::DataModel *dm = mOptions.Place.empty() ? engine.NewPlace() : LoadPlace(engine);
        engine.SwitchDataModel(dm);

        // Left alive at exit: the engine closes its renderer inside Exec, before this app is destroyed
        auto *bridge = new Gtr::Bridge(engine, *dm, mOptions.Bridge);
        if (!bridge->Open()) {
            std::cerr << "gtr-guest: the bridge could not start; running without it\n";
            return;
        }
        // Left alive at exit, as the bridge is
        auto *pacer = new FramePacer(mOptions.FrameRate > 0 ? mOptions.FrameRate : DisplayFrameRate());
        // The host's "perf" message can change it while the guest runs; 0 is back to the display's rate
        bridge->SetFrameRateHandler([pacer](int frameRate) { pacer->SetRate(frameRate > 0 ? frameRate : DisplayFrameRate()); });
        engine.GetTaskScheduler()->Enqueue({ sPaceJobPriority, "GtrPace", [pacer]() { pacer->Wait(); pacer->FollowHost(); } });
        engine.GetTaskScheduler()->Enqueue({ sReceiveJobPriority, "GtrReceive", [bridge]() { bridge->Receive(); } });
        engine.GetTaskScheduler()->Enqueue({ sDrawJobPriority, "GtrDraw", [bridge]() { bridge->Draw(); } });
        engine.GetTaskScheduler()->Enqueue({ sBridgeJobPriority, "GtrBridge", [bridge]() { bridge->Tick(); } });
    }
private:
    // Engine::LoadPlace with one step added: the map is taken out before any script runs or any character spawns
    Vanadium::DataModel *LoadPlace(Vanadium::Engine &engine) {
        using namespace Vanadium;
        DataModel *dm = engine.SpawnDataModel(sDataModelName);
        if (!BaseSerializer::LoadFile(Content(mOptions.Place).GetLocalPath(), dm))
            Log::Print("Gtr", "The guest starts empty because \"{}\" could not be loaded", mOptions.Place);
        else if (!mOptions.KeepMap)
            RemoveMap(*dm);
        dm->GetService<Lighting>()->MigrateTechnology();

        dm->GetService<Players>()->MakeLocalPlayer();
        dm->GetService<RunService>()->Run(Engine::GameCoreScripts);
        dm->GetService<Players>()->AdmitLocalPlayer();
        dm->SetLoaded(true);
        dm->Loaded.Fire();
        return dm;
    }

    GuestOptions mOptions;
};

bool ParseOptions(int argc, char **argv, GuestOptions &out, std::string &error) {
    for (int i = 1; i < argc; i++) {
        const std::string_view argument = argv[i];
        if (argument == "--metres-per-stud" || argument == "--port" || argument == "--fps") {
            if (i + 1 >= argc) {
                error = std::string(argument) + " needs a value";
                return false;
            }
            const std::string_view text = argv[++i];
            bool parsed = false;
            if (argument == "--port")
                parsed = std::from_chars(text.data(), text.data() + text.size(), out.Bridge.Port).ec == std::errc();
            else if (argument == "--fps")
                parsed = std::from_chars(text.data(), text.data() + text.size(), out.FrameRate).ec == std::errc() && out.FrameRate >= 0;
            else
                parsed = std::from_chars(text.data(), text.data() + text.size(), out.Bridge.MetresPerStud).ec == std::errc() &&
                         out.Bridge.MetresPerStud > 0.0f;
            if (!parsed) {
                error = std::string(argument) + " cannot be \"" + std::string(text) + "\"";
                return false;
            }
        } else if (argument == "--keep-map") {
            out.KeepMap = true;
        } else if (argument.starts_with("--")) {
            error = "unknown option " + std::string(argument);
            return false;
        } else {
            out.Place = std::filesystem::absolute(argument).string();
        }
    }
    return true;
}
}

int main(int argc, char **argv) {
    GuestOptions options;
    std::string error;
    if (!ParseOptions(argc, argv, options, error)) {
        std::cerr << "gtr-guest: " << error << "\nusage: gtr-guest [place.rbxl] [--keep-map] [--metres-per-stud 0.35] [--port 25610] [--fps 0]\n";
        return 2;
    }

    // The app reads the command line for places itself, and must not take the options for some
    GuestApp app({ .ArgCount = 1, .ArgVec = argv, .Title = "Vanadium (GTA passthrough guest)" }, std::move(options));
    return app.Exec();
}
