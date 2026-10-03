// Draws the scene a second time from the host's camera, at the host's resolution, and publishes its colour and depth in the shared
// memory of shared/gtr_frame.h.
#pragma once

#include "Gfx/Renderer.h"
#include "gtr_frame.h"

#include <array>
#include <cstdint>
#include <memory>

namespace Vanadium {
class Camera;
}

namespace Gtr {
// The camera a frame was drawn from, in host space
struct HostPose {
    int64_t CameraId { 0 };
    double Position[3] {};
    float Right[3] {};
    float Forward[3] {};
    float Up[3] {};
    float FovY { 50.0f };
};

// A part of a picture, in pixels from its top left
struct PixelRect {
    uint32_t X { 0 };
    uint32_t Y { 0 };
    uint32_t Width { 0 };
    uint32_t Height { 0 };
};

class FrameExporter {
public:
    FrameExporter(Vanadium::Gfx::Renderer &renderer, float metresPerStud);

    bool Open();
    // Queues a frame; it reaches the shared memory a tick or two later, once the GPU has handed its pixels back
    // drawn is the part of the picture that anything is drawn in: only that much is copied out
    // light, when there is one, is a camera at the host's sun, which the shadow map is drawn from
    void Render(Vanadium::Camera *camera, uint32_t width, uint32_t height, const HostPose &pose, const PixelRect &drawn,
                Vanadium::Camera *light, const HostPose &lightPose);
    void Publish();
    int64_t GetPublishedCount() const;
private:
    enum class ReadbackState {
        Free,
        Pending,
        Ready,
        Failed
    };

    struct Readback {
        WGPUBuffer Buffer { nullptr };
        uint64_t Capacity { 0 };
        uint32_t Width { 0 };
        uint32_t Height { 0 };
        uint32_t RowBytes { 0 };
        // The part of the picture that was copied, whose rows RowBytes is the length of in the buffer
        PixelRect Rect;
        // The interface's picture, after the two layers in the buffer
        uint32_t GuiWidth { 0 };
        uint32_t GuiHeight { 0 };
        uint32_t GuiRowBytes { 0 };
        // The shadow map, after the interface in the buffer; 0 without one
        uint32_t LightSize { 0 };
        HostPose LightPose;
        HostPose Pose;
        double GuestSeconds { 0.0 };
        uint64_t Order { 0 };
        ReadbackState State { ReadbackState::Free };
    };

    bool OpenMapping();
    void CreateTargets(uint32_t width, uint32_t height);
    void Write(const Readback &readback, const uint8_t *pixels);

    Vanadium::Gfx::Renderer &mRenderer;
    float mMetresPerStud;

    // Never destroyed: the renderer whose resources it borrows is closed before anything of the app's is
    Vanadium::Gfx::SceneView *mView { nullptr };
    WGPUShaderModule mShader { nullptr };
    WGPUBindGroupLayout mBindGroupLayout { nullptr };
    WGPURenderPipeline mColorPipeline { nullptr };
    WGPURenderPipeline mDepthPipeline { nullptr };
    WGPUBuffer mUniformBuffer { nullptr };
    WGPUBindGroup mBindGroup { nullptr };
    WGPUTextureView mBoundColor { nullptr };
    WGPUTextureView mBoundViewDepth { nullptr };
    WGPUTextureView mBoundCoverage { nullptr };
    WGPUTexture mColorTexture { nullptr };
    WGPUTextureView mColorView { nullptr };
    WGPUTexture mDepthTexture { nullptr };
    WGPUTextureView mDepthView { nullptr };
    uint32_t mWidth { 0 };
    uint32_t mHeight { 0 };
    // The guest's interface drawn again by itself, over nothing, at the size of the guest's window
    WGPUTexture mGuiTexture { nullptr };
    WGPUTextureView mGuiView { nullptr };
    uint32_t mGuiWidth { 0 };
    uint32_t mGuiHeight { 0 };
    // The guest seen from the host's sun: a view of its own, and its depth packed as the frame's is
    Vanadium::Gfx::SceneView *mLightView { nullptr };
    WGPUBindGroup mLightBindGroup { nullptr };
    WGPUTextureView mLightBoundColor { nullptr };
    WGPUTextureView mLightBoundViewDepth { nullptr };
    WGPUTexture mLightTexture { nullptr };
    WGPUTextureView mLightTextureView { nullptr };

    std::array<Readback, GTR_FRAME_SLOTS> mReadbacks;
    uint64_t mNextOrder { 1 };

    void *mMapping { nullptr };
    GtrFrameHeader *mHeader { nullptr };
};
}
