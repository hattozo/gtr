#include "Bodies.h"

#include "DataModel/Player/Backpack.h"
#include "DataModel/Camera.h"
#include "DataModel/DataModel.h"
#include "DataModel/Tool/HopperBin.h"
#include "DataModel/Value/StringValue.h"
#include "DataType/Ray.h"
#include "Input/InputSystem.h"
#include "Physics/World.h"
#include "DataModel/Folder.h"
#include "DataModel/Humanoid/Humanoid.h"
#include "DataModel/Model.h"
#include "DataModel/PartInstance.h"
#include "DataModel/Player/Player.h"
#include "DataModel/Service/Players.h"
#include "DataModel/Workspace.h"
#include "DataType/CFrame.h"
#include "DataType/Color3.h"
#include "DataType/SpatialQuery.h"

#include <nlohmann/json.hpp>

#include <format>

using namespace Vanadium;
using namespace Gtr;
using Json = nlohmann::json;

namespace {
constexpr const char *sFolderName = "HostBodies";
// A body in a message: its id and kind, its centre, its forward and up, its size along its right, forward and up, its health,
// its velocity
constexpr size_t sBodyNumbers = 18;
// What a person's health is out of, on both sides of the link
constexpr float sFullHealth = 100.0f;
// A part of the guest's pushes a body when it hits it at this speed or more (metres a second), and then not again for a while
constexpr float sPushSpeed = 2.0f;
constexpr std::chrono::milliseconds sPushInterval(300);
// What a part weighs for the host, by its mass in the guest: a classic brick comes to some 40 kg
constexpr float sKilogramsPerMass = 6.0f;
// The guest's loose parts going this fast (metres a second) or faster are the host's to look ahead of, so many at most; the
// slab put in the way of one is this large (studs; the last is its thickness) and stays this long after the host's last word
constexpr float sProbeSpeed = 6.0f;
constexpr size_t sMostProbes = 16;
constexpr const char *sWallFolderName = "HostWalls";
const Vector3 sWallSize(10.0f, 10.0f, 2.0f);
constexpr std::chrono::milliseconds sWallLasts(1200);
// A StringValue of this name under a body's part holds the body's id
constexpr const char *sIdName = "HostId";
// A box this far (studs) from where the host put it, or turned past this (the cosine of the angle), has been dragged; the host
// is not heard on it for this long after
constexpr float sDraggedStuds = 0.03f;
constexpr float sDraggedTurn = 0.9999f;
constexpr std::chrono::milliseconds sHeldFor(350);
// A copy that has rested this long has been let go of; a hammered body is not made again for this long
constexpr std::chrono::milliseconds sCopyRests(300);
constexpr std::chrono::seconds sDeleteSeconds(3);
constexpr int sLockedSkips = 16;
constexpr float sHoverReach = 2000.0f;
const Color3 sHoverColor(0.05f, 0.65f, 1.0f);
// A vehicle running into something of the guest's: from this speed (metres a second), no oftener than this, weighing this
// much, and bouncing back by this much of the speed it came at
constexpr float sCrashSpeed = 1.0f;
constexpr std::chrono::milliseconds sCrashInterval(120);
constexpr float sVehicleKilograms = 1500.0f;
constexpr float sCrashBounce = 0.15f;
// The character walking against a body: how near its middle has to be to the box (studs; its own half width and a little),
// what it weighs, and how much of that a person feels, who is to be jostled and not knocked down
constexpr float sWalkReach = 1.9f;
constexpr float sCharacterKilograms = 70.0f;
constexpr float sShoveOfPerson = 1.0f;
// A person is shoved again this soon, so that it is slid along and not knocked in steps
constexpr std::chrono::milliseconds sWalkInterval(80);
const Color3 sKindColors[] = { Color3(0.95f, 0.75f, 0.2f), Color3(0.2f, 0.6f, 0.95f), Color3(0.95f, 0.3f, 0.3f), Color3(0.95f, 0.3f, 0.75f) };
}

HostBodies::HostBodies(DataModel &dataModel, InputSystem *input, const double *origin, float metresPerStud) :
    mInput(input),
    mDataModel(dataModel),
    mOrigin(origin),
    mMetresPerStud(metresPerStud)
{}

Instance *HostBodies::GetFolder() {
    if (mFolder == nullptr || mFolder->Target == nullptr) {
        auto *folder = mDataModel.GetWorkspace()->CreateChild<Folder>();
        folder->SetName(sFolderName);
        mFolder = folder->GetHandle();
    }
    return mFolder->Target;
}

const Instance *HostBodies::GetFolderInstance() const {
    return mFolder != nullptr ? mFolder->Target : nullptr;
}

// The Humanoid of a person's model, that keeps the person's health: what the place's tools hurt. It neither walks the part
// nor dies for want of a head. A blow to it reaches the host as a blow to the person (Tick), and the host's word on the
// person's health comes back to it (Handle).
void HostBodies::SetRider(Body &body, bool riding) {
    auto *model = body.Model != nullptr ? body.Model->Target : nullptr;
    Instance *had = body.Humanoid != nullptr ? body.Humanoid->Target : nullptr;
    if (model == nullptr || riding == (had != nullptr))
        return;
    if (!riding) {
        had->Destroy();
        body.Humanoid = nullptr;
        return;
    }
    auto *humanoid = model->CreateChild<Humanoid>();
    humanoid->SetRequiresNeck(false);
    humanoid->SetEvaluateStateMachine(false);
    humanoid->SetDisplayDistanceType(HumanoidDisplayDistanceType::None);
    humanoid->SetMaxHealth(sFullHealth);
    humanoid->SetHealth(sFullHealth);
    body.Health = sFullHealth;
    body.Humanoid = humanoid->GetHandle();
}

void HostBodies::Remove(int id) {
    const auto found = mBodies.find(id);
    if (found == mBodies.end())
        return;
    Body &body = found->second;
    body.Touched.Disconnect();
    for (const auto &handle : { body.Model, body.Part }) {
        if (handle != nullptr && handle->Target != nullptr)
            handle->Target->Destroy();
    }
    mBodies.erase(found);
}

void HostBodies::Clear() {
    while (!mBodies.empty())
        Remove(mBodies.begin()->first);
    mPushed.clear();
}

void HostBodies::Show(bool visible) {
    mVisible = visible;
    for (const auto &[id, body] : mBodies) {
        if (auto *part = static_cast<PartInstance *>(body.Part->Target))
            part->SetTransparency(visible ? 0.5f : 1.0f);
    }
}

void HostBodies::Handle(const Json &message) {
    const float studs = 1.0f / mMetresPerStud;
    if (const auto set = message.find("set"); set != message.end() && set->is_array()) {
        for (const Json &entry : *set) {
            if (!entry.is_array() || entry.size() != sBodyNumbers)
                continue;
            double n[sBodyNumbers];
            bool numbers = true;
            for (size_t i = 0; i < sBodyNumbers && numbers; i++) {
                numbers = entry[i].is_number();
                if (numbers)
                    n[i] = entry[i].get<double>();
            }
            if (!numbers)
                continue;
            const int id = (int)n[0];
            const auto kind = (Kind)std::clamp((int)n[1], 0, 3);

            // One the hammer took is not made again while the host is still doing away with it
            if (mDeleted.contains(id))
                continue;
            // One that is now another kind (a person who has got into a vehicle, or out of one) is made again
            if (const auto known = mBodies.find(id); known != mBodies.end() && known->second.What != kind)
                Remove(id);
            Body &body = mBodies[id];
            auto *part = body.Part != nullptr ? static_cast<PartInstance *>(body.Part->Target) : nullptr;
            if (part == nullptr) {
                body.What = kind;
                part = new PartInstance();
                part->SetAnchored(true);
                // Not locked: the place's building tools take hold of a body as of a brick, and the host's thing follows
                part->SetTransparency(mVisible ? 0.5f : 1.0f);
                part->SetColor(sKindColors[(int)kind]);
                // Smooth all round: a part's studs and inlets join it to what it rests against (another box, a brick), and
                // a joined box is held where it was joined, which read as a tool dragging it
                for (const NormalId face : { NormalId::Right, NormalId::Top, NormalId::Back, NormalId::Left, NormalId::Bottom, NormalId::Front })
                    part->SetSurface(face, SurfaceType::Smooth);
                body.Part = part->GetHandle();
                // Which of the host's things it is, kept on the part so that a copy the clone tool makes of it says so too
                auto *name = part->CreateChild<StringValue>();
                name->SetName(sIdName);
                name->SetValue(std::to_string(id));
                Instance *parent = GetFolder();
                if (kind == Kind::Rider)
                    part->SetCanCollide(false);
                if (kind == Kind::Person || kind == Kind::Rider) {
                    // As a character is to the place's tools: a model with a part and a Humanoid in it (see SetRider)
                    auto *model = parent->CreateChild<Model>();
                    model->SetName("HostPerson");
                    body.Model = model->GetHandle();
                    SetRider(body, true);
                    part->SetName("Torso");
                    parent = model;
                } else {
                    part->SetName(kind == Kind::Vehicle ? "HostVehicle" : "HostObject");
                }
                body.Touched = part->Touched.Connect([this, id](BasePart *other) { OnTouched(id, other); });
                part->SetParent(parent);
            }

            // The host's right, forward and up are the part's X, -Z and Y
            const Vector3 forward((float)n[5], (float)n[7], (float)-n[6]), up((float)n[8], (float)n[10], (float)-n[9]);
            const Vector3 centre = Vector3((float)(n[2] - mOrigin[0]), (float)(n[4] - mOrigin[2]), (float)-(n[3] - mOrigin[1])) * studs;
            const Vector3 back(-forward.X, -forward.Y, -forward.Z);
            // A body a tool has hold of stays where the tool has it
            if (std::chrono::steady_clock::now() >= body.HeldUntil) {
                part->SetSize(Vector3((float)n[11], (float)n[13], (float)n[12]) * studs);
                body.Told = CFrame::FromMatrix(centre, up.Cross(back), up, back);
                part->SetCFrame(body.Told);
                body.Velocity = Vector3((float)n[15], (float)n[17], (float)-n[16]) * studs;
            }

            if (auto *humanoid = body.Humanoid != nullptr ? static_cast<Humanoid *>(body.Humanoid->Target) : nullptr) {
                const float health = std::clamp((float)n[14], 0.0f, sFullHealth);
                // Only what the host changed: what the guest did to it is on its way to the host and will come back
                if (health != body.Health && humanoid->GetHealth() == body.Health) {
                    humanoid->SetHealth(health);
                    body.Health = health;
                }
            }
        }
    }
    if (const auto keep = message.find("keep"); keep != message.end() && keep->is_array()) {
        std::vector<int> gone;
        for (const auto &[id, body] : mBodies) {
            if (std::ranges::find(*keep, Json(id)) == keep->end())
                gone.push_back(id);
        }
        for (const int id : gone)
            Remove(id);
        // Parts that have gone since they pushed something
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(mPushed, [now](const auto &pushed) { return now - pushed.second > sPushInterval * 10; });
        std::erase_if(mDeleted, [now](const auto &deleted) { return now - deleted.second > sDeleteSeconds; });
    }
}

// Something of the guest's has run into a body: the host's thing is pushed as that would push it
void HostBodies::OnTouched(int id, BasePart *other) {
    if (other == nullptr || other->GetAnchored() || (mFolder != nullptr && mFolder->Target != nullptr && other->IsDescendantOf(mFolder->Target)))
        return;
    // Not by the character walking into it: the character is stopped by the box, and that is all
    const Player *player = mDataModel.GetService<Players>()->GetLocalPlayer();
    if (const Model *character = player != nullptr ? player->GetCharacter() : nullptr; character != nullptr && other->IsDescendantOf(character))
        return;
    Vector3 velocity = other->GetAssemblyLinearVelocity() * mMetresPerStud;
    if (velocity.Magnitude() < sPushSpeed)
        return;
    // By the time the touch is told the part has bounced off the face it hit: its speed away from that face is the speed it
    // came in with
    const auto found = mBodies.find(id);
    if (const auto *part = found != mBodies.end() ? static_cast<const BasePart *>(found->second.Part->Target) : nullptr) {
        const CFrame &frame = part->GetCFrame();
        const Vector3 local = frame.PointToObjectSpace(other->GetPosition()), half = part->GetSize() * 0.5f;
        const float out[3] = { local.X / half.X, local.Y / half.Y, local.Z / half.Z };
        const Vector3 axes[3] = { frame.RightVector(), frame.UpVector(), frame.LookVector() * -1.0f };
        int face = 0;
        for (int i = 1; i < 3; i++) {
            if (std::abs(out[i]) > std::abs(out[face]))
                face = i;
        }
        const Vector3 outward = axes[face] * (out[face] < 0.0f ? -1.0f : 1.0f);
        if (const float away = velocity.Dot(outward); away > 0.0f)
            velocity = velocity - outward * (2.0f * away);
    }
    const auto now = std::chrono::steady_clock::now();
    if (const auto pushed = mPushed.find(other); pushed != mPushed.end() && now - pushed->second < sPushInterval)
        return;
    mPushed[other] = now;

    const Vector3 at = other->GetPosition() * mMetresPerStud;
    const Vector3 impulse = velocity * (other->GetMass() * sKilogramsPerMass);
    // Guest (x, y, z) is host (x, -z, y)
    mOutgoing.push_back(std::format("{{\"t\":\"impulse\",\"id\":{},\"at\":[{:.3f},{:.3f},{:.3f}],\"impulse\":[{:.2f},{:.2f},{:.2f}]}}", id,
                                    mOrigin[0] + at.X, mOrigin[1] - at.Z, mOrigin[2] + at.Y, impulse.X, -impulse.Z, impulse.Y));
}

std::vector<std::string> HostBodies::Tick(const Instance *ground) {
    for (auto &[id, body] : mBodies) {
        const auto *humanoid = body.Humanoid != nullptr ? static_cast<const Humanoid *>(body.Humanoid->Target) : nullptr;
        if (humanoid == nullptr)
            continue;
        // A tool of the place's has hurt the person: the host is told by how much
        const float health = humanoid->GetHealth();
        if (health < body.Health - 0.01f)
            mOutgoing.push_back(std::format("{{\"t\":\"hurt\",\"id\":{},\"damage\":{:.2f}}}", id, body.Health - health));
        body.Health = health;
    }
    FollowTools();
    ReportProjectiles();
    OutlineHovered();
    PushByWalking();
    CrashVehicles(ground);
    std::vector<std::string> lines;
    lines.swap(mOutgoing);
    return lines;
}

// The guest has ground only round its character, bodies only near it, and of the host's walls nothing: a rocket or a ball
// flies through all the rest. So the host is told of the guest's fast loose parts, where each is and how it goes, and
// answers with what of its world lies in the way (HandleWalls).
void HostBodies::ReportProjectiles() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = mWalls.begin(); it != mWalls.end();) {
        if (now < it->second.Until) {
            ++it;
            continue;
        }
        if (it->second.Part->Target != nullptr)
            it->second.Part->Target->Destroy();
        it = mWalls.erase(it);
    }

    std::unordered_map<const BasePart *, int> flying;
    std::string list;
    for (Instance *child : mDataModel.GetWorkspace()->ViewChildren()) {
        auto *part = dynamic_cast<BasePart *>(child);
        if (part == nullptr || part->GetAnchored() || !part->GetCanCollide() || flying.size() >= sMostProbes)
            continue;
        const Vector3 velocity = part->GetAssemblyLinearVelocity() * mMetresPerStud;
        if (velocity.Magnitude() < sProbeSpeed)
            continue;
        const auto known = mProbed.find(part);
        const int id = known != mProbed.end() ? known->second : mNextProbe++;
        const Vector3 at = part->GetPosition() * mMetresPerStud;
        list += std::format("{}{},{:.3f},{:.3f},{:.3f},{:.2f},{:.2f},{:.2f}", flying.empty() ? "" : ",", id, mOrigin[0] + at.X, mOrigin[1] - at.Z,
                            mOrigin[2] + at.Y, velocity.X, -velocity.Z, velocity.Y);
        flying[part] = id;
    }
    if (!flying.empty())
        mOutgoing.push_back(std::format("{{\"t\":\"probes\",\"n\":{},\"list\":[{}]}}", flying.size(), list));
    mProbed.swap(flying);
}

// For each of the guest's fast things the host found something in the way of: where, and which way that surface faces. A
// slab is put there, behind the surface, for the thing to hit, as it would have hit the host's wall or ground.
void HostBodies::HandleWalls(const Json &message) {
    const auto list = message.find("list");
    if (list == message.end() || !list->is_array())
        return;
    if (mWallFolder == nullptr || mWallFolder->Target == nullptr) {
        auto *folder = mDataModel.GetWorkspace()->CreateChild<Folder>();
        folder->SetName(sWallFolderName);
        mWallFolder = folder->GetHandle();
    }
    const float studs = 1.0f / mMetresPerStud;
    for (size_t i = 0; i + 6 < list->size(); i += 7) {
        double n[7];
        bool numbers = true;
        for (size_t j = 0; j < 7 && numbers; j++) {
            numbers = (*list)[i + j].is_number();
            if (numbers)
                n[j] = (*list)[i + j].get<double>();
        }
        if (!numbers)
            continue;
        Wall &wall = mWalls[(int)n[0]];
        auto *part = wall.Part != nullptr ? static_cast<PartInstance *>(wall.Part->Target) : nullptr;
        if (part == nullptr) {
            part = new PartInstance();
            part->SetName("HostWall");
            part->SetAnchored(true);
            part->SetLocked(true);
            part->SetTransparency(1.0f);
            part->SetSize(sWallSize);
            wall.Part = part->GetHandle();
            part->SetParent(mWallFolder->Target);
        }
        // The slab's back is the way the surface faces, and its face is at the surface
        const Vector3 at = Vector3((float)(n[1] - mOrigin[0]), (float)(n[3] - mOrigin[2]), (float)-(n[2] - mOrigin[1])) * studs;
        Vector3 back((float)n[4], (float)n[6], (float)-n[5]);
        if (back.Magnitude() < 0.5f)
            back = Vector3(0.0f, 1.0f, 0.0f);
        back = back * (1.0f / back.Magnitude());
        const Vector3 aside = std::abs(back.Y) > 0.99f ? Vector3(1.0f, 0.0f, 0.0f) : Vector3(0.0f, 1.0f, 0.0f).Cross(back);
        const Vector3 right = aside * (1.0f / aside.Magnitude());
        part->SetCFrame(CFrame::FromMatrix(at - back * (sWallSize.Z * 0.5f), right, back.Cross(right), back));
        wall.Until = std::chrono::steady_clock::now() + sWallLasts;
    }
}

// A box's place and turn in the host's terms, as a message: its centre, its forward and its up
std::string HostBodies::PoseMessage(const char *type, int id, const BasePart &part) const {
    const CFrame &frame = part.GetCFrame();
    const Vector3 at = frame.translation * mMetresPerStud, forward = frame.LookVector(), up = frame.UpVector();
    return std::format("{{\"t\":\"{}\",\"id\":{},\"pos\":[{:.3f},{:.3f},{:.3f}],\"fwd\":[{:.4f},{:.4f},{:.4f}],\"up\":[{:.4f},{:.4f},{:.4f}]}}", type, id,
                       mOrigin[0] + at.X, mOrigin[1] - at.Z, mOrigin[2] + at.Y, forward.X, -forward.Z, forward.Y, up.X, -up.Z, up.Y);
}

// What the place's building tools have done to the bodies: the old HopperBins take hold of a part, and a body's box is one. A
// box that is not where the host last put it has been dragged, and the host moves its thing there; a box that is gone was
// hammered, and the host does away with its thing; and a copy of a box, which the clone tool leaves in the workspace, becomes
// a copy of the thing once the tool has let go of it.
void HostBodies::FollowTools() {
    const auto now = std::chrono::steady_clock::now();
    const bool building = Building();
    std::vector<int> hammered;
    for (auto &[id, body] : mBodies) {
        auto *part = static_cast<BasePart *>(body.Part->Target);
        if (part == nullptr || part->GetParent() == nullptr) {
            hammered.push_back(id);
            continue;
        }
        const CFrame &frame = part->GetCFrame();
        if ((frame.translation - body.Told.translation).Magnitude() > sDraggedStuds || frame.LookVector().Dot(body.Told.LookVector()) < sDraggedTurn ||
            frame.UpVector().Dot(body.Told.UpVector()) < sDraggedTurn) {
            // Only a building tool moves the host's things. A box moved by anything else is put back where the host has the
            // thing: told to the host, every frame, it held the host's people walking on the spot and its traffic stopped
            if (!building) {
                part->SetCFrame(body.Told);
                continue;
            }
            body.Told = frame;
            body.HeldUntil = now + sHeldFor;
            body.Velocity = Vector3(0.0f, 0.0f, 0.0f);
            mOutgoing.push_back(PoseMessage("move", id, *part));
        }
    }
    for (const int id : hammered) {
        mOutgoing.push_back(std::format("{{\"t\":\"delete\",\"id\":{}}}", id));
        mDeleted[id] = now;
        Remove(id);
    }

    // The clone tool's copies are parts in the workspace itself that say which body they are a copy of
    for (Instance *child : mDataModel.GetWorkspace()->ViewChildren()) {
        auto *part = dynamic_cast<BasePart *>(child);
        if (part == nullptr || part->FindFirstChild(sIdName) == nullptr ||
            std::ranges::find(mCopies, part->GetHandle(), &Copy::Part) != mCopies.end())
            continue;
        mCopies.push_back({ part->GetHandle(), part->GetCFrame(), now });
    }
    for (size_t i = 0; i < mCopies.size();) {
        Copy &copy = mCopies[i];
        auto *part = static_cast<BasePart *>(copy.Part->Target);
        if (part == nullptr || part->GetParent() == nullptr) {
            mCopies.erase(mCopies.begin() + (ptrdiff_t)i);
            continue;
        }
        if ((part->GetCFrame().translation - copy.At.translation).Magnitude() > sDraggedStuds) {
            copy.At = part->GetCFrame();
            copy.Since = now;
        }
        if (now - copy.Since >= sCopyRests) {
            const auto *name = dynamic_cast<const StringValue *>(part->FindFirstChild(sIdName));
            if (name != nullptr)
                mOutgoing.push_back(PoseMessage("copy", std::atoi(name->GetValue().c_str()), *part));
            part->Destroy();
            mCopies.erase(mCopies.begin() + (ptrdiff_t)i);
            continue;
        }
        i++;
    }
}

// Whether one of the place's building tools (a HopperBin) is out
bool HostBodies::Building() const {
    Player *player = mDataModel.GetService<Players>()->GetLocalPlayer();
    const Instance *backpack = player != nullptr ? player->FindFirstChildWhichIsA<Backpack>() : nullptr;
    for (const Instance *item : backpack != nullptr ? backpack->ViewChildren() : std::span<Instance *const>()) {
        if (const auto *bin = dynamic_cast<const HopperBin *>(item); bin != nullptr && bin->GetActive())
            return true;
    }
    return false;
}

// With one of the building tools out, the body the pointer is on is outlined: its box is unseen, so without this there is no
// telling what the tool would take hold of.
void HostBodies::OutlineHovered() {
    BasePart *hovered = nullptr;
    Player *player = mDataModel.GetService<Players>()->GetLocalPlayer();
    const bool building = Building();
    Workspace *workspace = mDataModel.GetWorkspace();
    Camera *camera = workspace->FindCurrentCamera();
    Physics::World *world = workspace->GetPhysicsWorld();
    const Instance *folder = mFolder != nullptr ? mFolder->Target : nullptr;
    if (building && camera != nullptr && world != nullptr && mInput != nullptr && folder != nullptr) {
        const Vector2 &pointer = mInput->GetMousePosition();
        const Ray ray = camera->ViewportPointToRay(pointer.X, pointer.Y, 0.0f);
        std::vector<Instance *> ignored;
        if (Model *character = player->GetCharacter())
            ignored.push_back(character);
        // Past what is locked, as the tools themselves look: the ground's tiles are
        for (int i = 0; i < sLockedSkips; i++) {
            QueryFilter filter;
            filter.SetFilterInstances(ignored);
            const std::optional<RaycastResult> hit = world->Raycast(ray.Origin, ray.Direction * sHoverReach, filter);
            if (!hit.has_value() || hit->Instance == nullptr || hit->Instance->Target == nullptr)
                break;
            auto *part = static_cast<BasePart *>(hit->Instance->Target);
            if (!part->GetLocked()) {
                hovered = part->IsDescendantOf(folder) ? part : nullptr;
                break;
            }
            ignored.push_back(part);
        }
    }
    mDataModel.SetPartOutline(this, hovered, sHoverColor);
}

// One of the host's vehicles running into something of the guest's. The vehicle's box is only carried to where the host says
// the vehicle is, so by itself the guest's physics would have the vehicle go on as if nothing were there, shouldering the
// thing out of its way, or passing through it if it is anchored. Here the two are made to collide as bodies do: by their
// masses, the thing is knocked on and the vehicle slowed, and the host is told by how much to slow its vehicle; against
// something anchored the vehicle takes it all, and is stopped.
void HostBodies::CrashVehicles(const Instance *ground) {
    const Player *player = mDataModel.GetService<Players>()->GetLocalPlayer();
    const Model *character = player != nullptr ? player->GetCharacter() : nullptr;
    const Instance *bodies = mFolder != nullptr ? mFolder->Target : nullptr;
    const auto now = std::chrono::steady_clock::now();
    for (auto &[id, body] : mBodies) {
        const auto *box = body.What == Kind::Vehicle ? static_cast<const BasePart *>(body.Part->Target) : nullptr;
        if (box == nullptr || body.Velocity.Magnitude() * mMetresPerStud < sCrashSpeed || now - body.Crashed < sCrashInterval / 4)
            continue;
        const CFrame &frame = box->GetCFrame();
        const Vector3 half = box->GetSize() * 0.5f;
        const Vector3 axes[3] = { frame.RightVector(), frame.UpVector(), frame.LookVector() * -1.0f };
        Vector3 change(0.0f, 0.0f, 0.0f), where(0.0f, 0.0f, 0.0f);
        float hardest = 0.0f;
        for (Instance *found : mDataModel.GetWorkspace()->GetPartBoundsInBox(frame, box->GetSize(), OverlapParams())) {
            auto *part = dynamic_cast<BasePart *>(found);
            if (part == nullptr || !part->GetCanCollide() || (bodies != nullptr && part->IsDescendantOf(bodies)) ||
                (mWallFolder != nullptr && mWallFolder->Target != nullptr && part->IsDescendantOf(mWallFolder->Target)) ||
                (ground != nullptr && part->IsDescendantOf(ground)) || (character != nullptr && part->IsDescendantOf(character)))
                continue;
            // The side of the vehicle the thing is at, which is the way the two push each other
            const Vector3 local = frame.PointToObjectSpace(part->GetPosition());
            const float out[3] = { local.X / half.X, local.Y / half.Y, local.Z / half.Z };
            int face = 0;
            for (int i = 1; i < 3; i++) {
                if (std::abs(out[i]) > std::abs(out[face]))
                    face = i;
            }
            const Vector3 outward = axes[face] * (out[face] < 0.0f ? -1.0f : 1.0f);
            const Vector3 partVelocity = part->GetAnchored() ? Vector3(0.0f, 0.0f, 0.0f) : part->GetAssemblyLinearVelocity();
            const float closing = (body.Velocity - partVelocity).Dot(outward);
            if (closing * mMetresPerStud < sCrashSpeed)
                continue;
            // How the change of speed is shared: all the vehicle's against what is anchored, else by the two masses
            const float partMass = part->GetMass() * sKilogramsPerMass;
            const float vehicleShare = part->GetAnchored() ? 1.0f : partMass / (partMass + sVehicleKilograms);
            change = change - outward * (closing * (1.0f + sCrashBounce) * vehicleShare);
            if (!part->GetAnchored()) {
                // Out of the vehicle at once, at its face: the box is carried, not pushed, so the physics would only have the
                // brick work its way out over some frames, inside the vehicle meanwhile
                const float depth = std::abs(out[face]) <= 1.0f ? (1.0f - std::abs(out[face])) * (face == 0 ? half.X : face == 1 ? half.Y : half.Z) : 0.0f;
                const Vector3 extent = part->GetSize() * 0.5f;
                const float own = std::abs(extent.X * part->GetCFrame().RightVector().Dot(outward)) + std::abs(extent.Y * part->GetCFrame().UpVector().Dot(outward)) +
                                  std::abs(extent.Z * part->GetCFrame().LookVector().Dot(outward));
                CFrame placed = part->GetCFrame();
                placed.translation = placed.translation + outward * (depth + own);
                part->SetCFrame(placed);
                part->SetAssemblyLinearVelocity(partVelocity + outward * (closing * (1.0f + sCrashBounce) * (1.0f - vehicleShare)));
            }
            if (closing > hardest) {
                hardest = closing;
                where = part->GetPosition();
            }
        }
        if (hardest <= 0.0f)
            continue;
        body.Crashed = now;
        // The vehicle is taken to go on at its new speed until the host says otherwise, so it isn't stopped twice for one wall
        body.Velocity = body.Velocity + change;
        const Vector3 dv = change * mMetresPerStud, at = where * mMetresPerStud;
        mOutgoing.push_back(std::format("{{\"t\":\"crash\",\"id\":{},\"dv\":[{:.2f},{:.2f},{:.2f}],\"at\":[{:.3f},{:.3f},{:.3f}],\"hard\":{:.2f}}}", id,
                                        dv.X, -dv.Z, dv.Y, mOrigin[0] + at.X, mOrigin[1] - at.Z, mOrigin[2] + at.Y, hardest * mMetresPerStud));
    }
}

// The character walking into one of the host's loose things shoves it, as the host's own player would: a door swings open,
// a cone is kicked along. A box only stops the character, and a touch is told once, so this looks each frame for the
// objects and people the character is walking against.
void HostBodies::PushByWalking() {
    const Player *player = mDataModel.GetService<Players>()->GetLocalPlayer();
    Model *character = player != nullptr ? player->GetCharacter() : nullptr;
    auto *humanoid = character != nullptr ? character->FindFirstChildWhichIsA<Humanoid>() : nullptr;
    // The Humanoid's root: a classic R6 model's PrimaryPart is its head
    const BasePart *root = humanoid != nullptr ? humanoid->GetRootPart() : nullptr;
    if (humanoid == nullptr || root == nullptr || humanoid->GetHealth() <= 0.0f)
        return;
    const Vector3 heading = humanoid->GetMoveDirection();
    if (heading.Magnitude() < 0.5f)
        return;
    const auto now = std::chrono::steady_clock::now();
    const Vector3 at = root->GetPosition();
    for (auto &[id, body] : mBodies) {
        // Not people: GTA's player, which is moved with the character, pushes them itself, as GTA's own player does, and a
        // person told to step aside as well was left walking on the spot. Nor vehicles, which a walker doesn't move
        const auto *part = body.What == Kind::Object ? static_cast<const BasePart *>(body.Part->Target) : nullptr;
        if (part == nullptr || now - body.Walked < (body.What == Kind::Person ? sWalkInterval : sPushInterval))
            continue;
        // Within reach of the box, level with it, and walking towards it
        const Vector3 local = part->GetCFrame().PointToObjectSpace(at), half = part->GetSize() * 0.5f;
        const Vector3 outside(std::max(std::abs(local.X) - half.X, 0.0f), std::max(std::abs(local.Y) - half.Y, 0.0f), std::max(std::abs(local.Z) - half.Z, 0.0f));
        if (outside.Magnitude() > sWalkReach || outside.Y > sWalkReach * 0.5f || heading.Dot(part->GetPosition() - at) <= 0.0f)
            continue;
        body.Walked = now;
        const Vector3 impulse = heading * (humanoid->GetWalkSpeed() * mMetresPerStud * sCharacterKilograms * (body.What == Kind::Person ? sShoveOfPerson : 1.0f));
        const Vector3 where = at * mMetresPerStud;
        mOutgoing.push_back(std::format("{{\"t\":\"impulse\",\"id\":{},\"walk\":true,\"at\":[{:.3f},{:.3f},{:.3f}],\"impulse\":[{:.2f},{:.2f},{:.2f}]}}", id,
                                        mOrigin[0] + where.X, mOrigin[1] - where.Z, mOrigin[2] + where.Y, impulse.X, -impulse.Z, impulse.Y));
    }
}
