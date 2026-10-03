// The host's moving things as the guest meets them: its vehicles, its people and its loose objects, each an invisible box that
// follows the thing it stands for. The guest's character walks into them and stands on them, and what the place's tools throw
// hits them. A person's box carries a Humanoid, so a tool that hurts a character hurts it by its own script; what the guest
// does to a box is told back to the host, which does it to the thing itself.
#pragma once

#include "DataModel/Tool/HopperBin.h"
#include "DataType/CFrame.h"
#include "DataType/Color3.h"
#include "DataType/RBXScriptConnection.h"
#include "DataType/Vector3.h"

#include <nlohmann/json_fwd.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Vanadium {
class BasePart;
class InputSystem;
class DataModel;
class Instance;
class Model;
struct InstanceHandle;
}

namespace Gtr {
class HostBodies {
public:
    // origin is where the host's world is placed in the guest's, which the bridge keeps
    HostBodies(Vanadium::DataModel &dataModel, Vanadium::InputSystem *input, const double *origin, float metresPerStud);

    // A "bodies" message: the bodies that are new or have moved, and now and then the list of all that are still there
    void Handle(const nlohmann::json &message);
    // A "walls" message: where the host's world is in the way of the guest's fast things
    void HandleWalls(const nlohmann::json &message);
    // What has happened to the bodies since the last call, as lines for the host
    // ground is the folder of the host's ground, which nothing runs into
    std::vector<std::string> Tick(const Vanadium::Instance *ground);
    void Clear();
    void Show(bool visible);
    size_t GetCount() const { return mBodies.size(); }
    bool IsShown() const { return mVisible; }
    // The folder the boxes are kept in, or null before there is one
    const Vanadium::Instance *GetFolderInstance() const;
private:
    // A rider is a person in one of the host's vehicles: a box at the vehicle's side that the guest's things pass through
    enum class Kind { Object = 0, Vehicle = 1, Person = 2, Rider = 3 };
    struct Body {
        Kind What { Kind::Object };
        std::shared_ptr<Vanadium::InstanceHandle> Part;
        // A person's: the model that holds its part and its Humanoid
        std::shared_ptr<Vanadium::InstanceHandle> Model;
        std::shared_ptr<Vanadium::InstanceHandle> Humanoid;
        // The health the host and the guest last agreed on
        float Health { 100.0f };
        Vanadium::RBXScriptConnection Touched;
        // When the character last shoved it by walking into it
        std::chrono::steady_clock::time_point Walked;
        // Where the host last said it is. The place's building tools move the box itself: a box that is somewhere else has
        // been dragged, and the host is told to put its thing there. Until then what the host says of it is not taken
        Vanadium::CFrame Told;
        std::chrono::steady_clock::time_point HeldUntil;
        // Until when a building tool counts as moving it: while it drags the box, and a little after
        std::chrono::steady_clock::time_point DraggedUntil;
        // A vehicle's: how fast it goes (studs a second, in the guest's space), and when it last ran into something
        Vanadium::Vector3 Velocity { 0.0f, 0.0f, 0.0f };
        std::chrono::steady_clock::time_point Crashed;
        // When the host was last told what is in front of it
        std::chrono::steady_clock::time_point Looked;
        // The box's colour as the guest last knew it: a place's tool that colours what it hits (the paintball gun) colours
        // the box, and a vehicle's is then painted so in GTA
        Vanadium::Color3 Paint;
    };

    void Remove(int id);
    void OnTouched(int id, Vanadium::BasePart *other);
    void PushByWalking();
    bool MeetsVehicles(const Vanadium::BasePart *part, const Vanadium::Instance *ground, const Vanadium::Model *character) const;
    void LookAhead(const Vanadium::Instance *ground);
    void CrashVehicles(const Vanadium::Instance *ground);
    void FollowTools();
    bool Building(std::optional<Vanadium::BinType> type = std::nullopt) const;
    void SetRider(Body &body, bool riding);
    void ReportProjectiles();
    void ReportPaint();
    void OutlineHovered();
    std::string PoseMessage(const char *type, int id, const Vanadium::BasePart &part) const;
    Vanadium::Instance *GetFolder();

    Vanadium::DataModel &mDataModel;
    const double *mOrigin;
    float mMetresPerStud;
    std::unordered_map<int, Body> mBodies;
    std::shared_ptr<Vanadium::InstanceHandle> mFolder;
    bool mVisible { false };
    std::vector<std::string> mOutgoing;
    // When each of the guest's parts last pushed a body, so that one that rests against it doesn't push every frame
    std::unordered_map<const Vanadium::BasePart *, std::chrono::steady_clock::time_point> mPushed;
    // Bodies the hammer has taken, which the host goes on listing until it has done away with them
    std::unordered_map<int, std::chrono::steady_clock::time_point> mDeleted;
    // Copies the clone tool has made of a body's box, with where and when each was last seen to move: once one rests, the
    // host is asked for a copy of the thing there
    struct Copy {
        std::shared_ptr<Vanadium::InstanceHandle> Part;
        Vanadium::CFrame At;
        std::chrono::steady_clock::time_point Since;
    };
    std::vector<Copy> mCopies;
    Vanadium::InputSystem *mInput;
    // The guest's fast things the host is asked about, by a number each, and the walls put in their way: one for each, moved
    // as the host answers and taken away when it has not answered for a while
    std::unordered_map<const Vanadium::BasePart *, int> mProbed;
    int mNextProbe { 1 };
    struct Wall {
        std::shared_ptr<Vanadium::InstanceHandle> Part;
        std::chrono::steady_clock::time_point Until;
    };
    std::unordered_map<int, Wall> mWalls;
    std::shared_ptr<Vanadium::InstanceHandle> mWallFolder;
};
}
