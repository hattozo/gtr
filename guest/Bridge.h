// Ties the guest to the host: takes the host's camera and commands off the link, keeps the guest's world placed in the host's, and
// has each frame exported.
//
// Host space is GTA's: metres, X east, Y north, Z up. Guest space is Vanadium's: studs, Y up. Both are right-handed, and a host
// point h lands at StudsPerMetre * (h.x - o.x, h.z - o.z, -(h.y - o.y)) for the origin o, which the host names once so the guest's
// coordinates stay small wherever in the host's world it plays.
#pragma once

#include "Bodies.h"
#include "FrameExporter.h"
#include "HostLink.h"

#include "DataType/CFrame.h"
#include "DataType/Vector3.h"

#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Vanadium {
class Camera;
class DataModel;
class Engine;
class Humanoid;
class Instance;
class Model;
struct InstanceHandle;
}

namespace Gtr {
struct BridgeOptions {
    // 0.35 puts a classic five-stud character at a GTA pedestrian's height
    float MetresPerStud { 0.35f };
    uint16_t Port { GTR_LINK_PORT };
};

class Bridge {
public:
    Bridge(Vanadium::Engine &engine, Vanadium::DataModel &dataModel, const BridgeOptions &options);

    bool Open();
    // Draws the host's picture of this frame; run before the frame's physics step
    void Draw();
    // Hears the host; run at the start of a frame
    void Receive();
    // Answers the host; run once the frame is done
    void Tick();
private:
    Vanadium::Vector3 PointToGuest(const double host[3]) const;
    static Vanadium::Vector3 DirectionToGuest(const float host[3]);
    void Handle(int client, const std::string &line);
    Vanadium::Model *GetCharacter() const;
    Vanadium::Humanoid *GetHumanoid() const;
    Vanadium::BasePart *GetRoot() const;
    Vanadium::CFrame GetRootFrame() const;
    void PlaceCharacter(const Vanadium::Vector3 &rootAt);
    void ReleaseStaleInput();
    void MatchWindow();
    void ReportState();
    void ReportExplosions();
    HostPose PoseToHost(const Vanadium::CFrame &frame, float fovY) const;
    Vanadium::CFrame GetShake();
    Vanadium::Instance *GetFolder(std::shared_ptr<Vanadium::InstanceHandle> &handle, const char *name);
    void PlaceSpawn(const double host[3], bool moveCharacter);
    void SetGround(const std::vector<double> &tiles, float cell, float depth);
    void DropGround(const std::vector<double> &cells, float cell);
    void ShowGround(bool visible);

    Vanadium::Engine &mEngine;
    Vanadium::DataModel &mDataModel;
    BridgeOptions mOptions;
    float mStudsPerMetre;
    HostLink mLink;
    FrameExporter mExporter;

    double mOrigin[3] {};
    bool mHasOrigin { false };
    HostPose mPose;
    uint32_t mWidth { 0 };
    uint32_t mHeight { 0 };
    bool mHasPose { false };
    std::shared_ptr<Vanadium::InstanceHandle> mCamera;
    std::shared_ptr<Vanadium::InstanceHandle> mBrickFolder;
    std::unordered_map<std::string, std::shared_ptr<Vanadium::InstanceHandle>> mBricks;
    // The host's world as the guest collides with it: one invisible anchored part for each cell of ground the host has probed,
    // keyed by the cell's column and row in the host's grid
    std::shared_ptr<Vanadium::InstanceHandle> mGroundFolder;
    std::unordered_map<uint64_t, std::shared_ptr<Vanadium::InstanceHandle>> mGround;
    // Tiles the host has dropped, kept to be used again
    std::vector<std::shared_ptr<Vanadium::InstanceHandle>> mSpareGround;
    bool mGroundVisible { false };
    // The host's vehicles, people and loose objects
    HostBodies mHostBodies;
    // Where characters appear, and what they stand on until the host's ground arrives
    std::shared_ptr<Vanadium::InstanceHandle> mSpawn;
    std::chrono::steady_clock::time_point mRateSince;
    int64_t mRatePublished { 0 };
    // The client that sends the camera: the host's script. Its keyboard and mouse play the place, and it is told where the
    // character and the place's camera are
    int mHostClient { 0 };
    std::chrono::steady_clock::time_point mHostHeardAt;
    std::set<int> mHeldKeys;
    std::set<int> mHeldButtons;
    uint32_t mWindowFor[2] {};
    std::optional<float> mHostClock;
    // With the host's sun known, the latitude that, with that clock, puts the guest's sun where the host's is
    std::optional<float> mHostLatitude;
    // The way to the host's sun, in the guest's space, while it has one, and the camera the shadow map is drawn from
    std::optional<Vanadium::Vector3> mSunDirection;
    std::shared_ptr<Vanadium::InstanceHandle> mLightCamera;
    void FitSun(const Vanadium::Vector3 &towardsSun);
    // The explosions the host has been told of, while they last
    std::vector<std::shared_ptr<Vanadium::InstanceHandle>> mExplosions;
    // Whether the picture is drawn from the place's own camera (the host follows it) or from one the host sends
    bool mOwnCamera { false };
    bool mRendererDrew { false };
    // Whether the place has been given what the guest adds to it: the character's looks and the building tools
    bool mPlaceDressed { false };
    // Where the bridge's time goes, by what it was doing, summed between two lines of the log
    std::map<std::string, double> mCosts;
    int mCostTicks { 0 };
    void DressPlace();
    void RunPlaceScript(const std::string &name);
    // The character riding one of the host's vehicles: held in the seat the host says, moved there over a moment when it
    // gets in
    bool mSeated { false };
    Vanadium::CFrame mSeat;
    Vanadium::CFrame mSeatFrom;
    std::chrono::steady_clock::time_point mSeatSince;
    void HoldSeat();
    void LeaveSeat(const double host[3]);
    void PassOnRequests();
    // The part of the camera's picture that anything the guest draws can be in
    PixelRect FindDrawn(const Vanadium::Camera &camera, uint32_t width, uint32_t height) const;
    int64_t mOwnFrames { 0 };
    // One of the host's own camera shakes, as host/shake_record.py wrote it down from the host: what the shake adds to a
    // camera, sample by sample, in metres to the camera's right, forward and up, and degrees of pitch, roll and yaw
    struct ShakeTrace {
        std::string Name;
        float Rate { 100.0f };
        std::vector<std::array<float, 6>> Samples;
    };
    std::vector<ShakeTrace> mShakeTraces;
    struct Shake {
        std::chrono::steady_clock::time_point Start;
        // Without a trace to play: the largest swing of a shake made up here
        float Degrees;
        // The trace played, or -1, and how strongly
        int Trace { -1 };
        float Amplitude { 1.0f };
    };
    std::vector<Shake> mShakes;
    void LoadShakes();
    // A shake for a blast of this radius in metres, this far from the camera
    void AddShake(float radius, float distance);
};
}
