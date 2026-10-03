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
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
// The host's picture is drawn just before the renderer draws the guest's own (9995): after the frame's scripts have placed
// the camera, before the physics step (9996) moves the character on from where that camera looks. The host is heard and
// answered once the frame is done.
// The host is heard before anything else in a frame (the renderer's own first job is 1), so that the frame acts on it
constexpr int sReceiveJobPriority = 0;
constexpr int sDrawJobPriority = 9994;
constexpr int sBridgeJobPriority = 9999;
constexpr const char *sDataModelName = "Passthrough";
// Mid-afternoon, until the host's own time of day drives the guest's lighting
constexpr float sDefaultClockTime = 14.0f;

struct GuestOptions {
    std::string Place;
    bool KeepMap { false };
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
        if (argument == "--metres-per-stud" || argument == "--port") {
            if (i + 1 >= argc) {
                error = std::string(argument) + " needs a value";
                return false;
            }
            const std::string_view text = argv[++i];
            bool parsed = false;
            if (argument == "--port")
                parsed = std::from_chars(text.data(), text.data() + text.size(), out.Bridge.Port).ec == std::errc();
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
        std::cerr << "gtr-guest: " << error << "\nusage: gtr-guest [place.rbxl] [--keep-map] [--metres-per-stud 0.35] [--port 25610]\n";
        return 2;
    }

    // The app reads the command line for places itself, and must not take the options for some
    GuestApp app({ .ArgCount = 1, .ArgVec = argv, .Title = "Vanadium (GTA passthrough guest)" }, std::move(options));
    return app.Exec();
}
