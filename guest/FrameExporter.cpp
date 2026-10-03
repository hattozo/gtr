#include "FrameExporter.h"

#include "DataModel/Camera.h"
#include "Gfx/FullscreenPass.h"
#include "Gfx/GuiRenderer.h"
#include "Gfx/SceneView.h"
#include "Util/Log.h"

#include <webgpu/wgpu.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

using namespace Vanadium;
using namespace Vanadium::Gfx;
using namespace Gtr;

namespace {
constexpr uint32_t sUniformBinding = 0;
constexpr uint32_t sColorBinding = 1;
constexpr uint32_t sViewDepthBinding = 2;
constexpr uint32_t sCoverageBinding = 3;
// WebGPU copies texture rows into buffers at a multiple of this many bytes
constexpr uint32_t sRowAlignment = 256;
constexpr uint32_t sBytesPerPixel = 4;
constexpr uint32_t sMinimumSize = 16;
constexpr const char *sGuestMutexName = "Local\\GtrPassthroughGuest";

struct PackUniforms {
    float MetresPerStud;
    float EmptyViewDepth;
    float EmptyDepth;
    float Padding;
};

// The view depth is negated where a part glows, and is the clear value where no part was drawn.
//
// The scene is antialiased, so a pixel on the outline of what was drawn is part sky: bright, and a pale rim around
// everything once the picture stands in the host's night. Such a pixel takes its colour from the pixels beside it that are
// wholly inside the outline; the host softens the edge again against its own picture.
const char *const sPackShader = R"(
struct PackUniforms {
    metres_per_stud: f32,
    empty_view_depth: f32,
    empty_depth: f32,
    padding: f32,
}

@group(0) @binding(0) var<uniform> pack: PackUniforms;
@group(0) @binding(1) var scene_color: texture_2d<f32>;
@group(0) @binding(2) var scene_view_depth: texture_2d<f32>;
// What the see-through parts cover where nothing opaque is, and how far off they are (SceneView's transparent coverage): the
// view is drawn without its sky, so there its color is theirs times their coverage
@group(0) @binding(3) var scene_coverage: texture_2d<f32>;

@vertex
fn vs_fullscreen(@builtin(vertex_index) vertex: u32) -> @builtin(position) vec4<f32> {
    var corners = array<vec2<f32>, 3>(vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0), vec2<f32>(-1.0, 3.0));
    return vec4<f32>(corners[vertex], 0.0, 1.0);
}

fn drawn(pixel: vec2<i32>) -> bool {
    let last = vec2<i32>(textureDimensions(scene_view_depth)) - vec2<i32>(1, 1);
    return abs(textureLoad(scene_view_depth, clamp(pixel, vec2<i32>(0, 0), last), 0).r) < pack.empty_view_depth;
}

fn inside(pixel: vec2<i32>) -> bool {
    return drawn(pixel) && drawn(pixel + vec2<i32>(1, 0)) && drawn(pixel - vec2<i32>(1, 0)) && drawn(pixel + vec2<i32>(0, 1)) &&
           drawn(pixel - vec2<i32>(0, 1));
}

@fragment
fn fs_color(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let pixel = vec2<i32>(position.xy);
    let color = textureLoad(scene_color, pixel, 0).rgb;
    if (!drawn(pixel)) {
        let coverage = textureLoad(scene_coverage, pixel, 0).a;
        if (coverage > 0.004) {
            return vec4<f32>(clamp(color / coverage, vec3<f32>(0.0), vec3<f32>(1.0)), coverage);
        }
        return vec4<f32>(color, 0.0);
    }
    if (inside(pixel)) {
        return vec4<f32>(color, 1.0);
    }
    var sum = vec3<f32>(0.0);
    var count = 0.0;
    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            let beside = pixel + vec2<i32>(x, y);
            if (inside(beside)) {
                sum += textureLoad(scene_color, beside, 0).rgb;
                count += 1.0;
            }
        }
    }
    // Something too thin to have an inside keeps the colour it has
    return vec4<f32>(select(color, sum / max(count, 1.0), count > 0.0), 1.0);
}

@fragment
fn fs_depth(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let view_depth = abs(textureLoad(scene_view_depth, vec2<i32>(position.xy), 0).r);
    if (view_depth >= pack.empty_view_depth) {
        // Nothing opaque: the nearest see-through part, if any covers the pixel
        let coverage = textureLoad(scene_coverage, vec2<i32>(position.xy), 0);
        return vec4<f32>(select(pack.empty_depth, coverage.r * pack.metres_per_stud, coverage.a > 0.004), 0.0, 0.0, 0.0);
    }
    return vec4<f32>(view_depth * pack.metres_per_stud, 0.0, 0.0, 0.0);
}
)";

void CreateReadableTarget(WGPUDevice device, const char *label, WGPUTextureFormat format, uint32_t width, uint32_t height,
                          WGPUTexture &texture, WGPUTextureView &view) {
    WGPUTextureDescriptor textureDesc {};
    textureDesc.label = Gfx::Str(label);
    textureDesc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    textureDesc.dimension = WGPUTextureDimension_2D;
    textureDesc.size = { width, height, 1 };
    textureDesc.format = format;
    textureDesc.mipLevelCount = 1;
    textureDesc.sampleCount = 1;
    texture = wgpuDeviceCreateTexture(device, &textureDesc);
    view = wgpuTextureCreateView(texture, nullptr);
}

void CopyToBuffer(WGPUCommandEncoder encoder, WGPUTexture texture, WGPUBuffer buffer, uint64_t offset, uint32_t rowBytes,
                  uint32_t width, uint32_t height, uint32_t fromX = 0, uint32_t fromY = 0) {
    WGPUTexelCopyTextureInfo source {};
    source.texture = texture;
    source.aspect = WGPUTextureAspect_All;
    source.origin = { fromX, fromY, 0 };

    WGPUTexelCopyBufferInfo destination {};
    destination.buffer = buffer;
    destination.layout.offset = offset;
    destination.layout.bytesPerRow = rowBytes;
    destination.layout.rowsPerImage = height;

    const WGPUExtent3D extent { width, height, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &extent);
}

double SteadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

FrameExporter::FrameExporter(Renderer &renderer, float metresPerStud) :
    mRenderer(renderer),
    mMetresPerStud(metresPerStud)
{}

bool FrameExporter::OpenMapping() {
    const int64_t stride = (int64_t)GTR_FRAME_MAX_WIDTH * GTR_FRAME_MAX_HEIGHT * sBytesPerPixel * 3 + (int64_t)GTR_LIGHT_SIZE * GTR_LIGHT_SIZE * sBytesPerPixel;
    const uint64_t bytes = GTR_FRAME_HEADER_BYTES + (uint64_t)stride * GTR_FRAME_SLOTS;
    // A mutex says whether another guest is running. The mapping can't: a host that read an earlier guest's frames still holds
    // it open, so it outlives that guest and is taken over by the next one
    CreateMutexA(nullptr, TRUE, sGuestMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Log::Print("Gtr", "Another guest already publishes frames; close it first");
        return false;
    }
    mMapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)(bytes >> 32), (DWORD)bytes, GTR_FRAME_MAPPING_NAME);
    if (mMapping == nullptr) {
        Log::Print("Gtr", "Could not create the frame mapping (error {})", (int)GetLastError());
        return false;
    }
    mPublishedEvent = CreateEventA(nullptr, FALSE, FALSE, GTR_FRAME_PUBLISHED_EVENT);
    mHeader = static_cast<GtrFrameHeader *>(MapViewOfFile(mMapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
    if (mHeader == nullptr) {
        Log::Print("Gtr", "Could not map the frame mapping (error {})", (int)GetLastError());
        return false;
    }

    mHeader->Version = GTR_FRAME_VERSION;
    mHeader->SlotCount = GTR_FRAME_SLOTS;
    mHeader->MaxWidth = GTR_FRAME_MAX_WIDTH;
    mHeader->MaxHeight = GTR_FRAME_MAX_HEIGHT;
    mHeader->SlotStride = stride;
    // Published only ever counts up, so a reader of an earlier guest's frames sees this guest's first as new
    mHeader->LatestSlot = -1;
    // Last, so a reader that sees the magic sees the rest
    std::atomic_thread_fence(std::memory_order_seq_cst);
    mHeader->Magic = GTR_FRAME_MAGIC;
    return true;
}

bool FrameExporter::Open() {
    GpuContext &gpu = mRenderer.Gpu;
    if (!gpu.IsValid() || !OpenMapping())
        return false;

    mShader = gpu.CreateShaderModule(sPackShader, "GtrPack");
    mBindGroupLayout = FullscreenPass::CreateBindGroupLayout(gpu.Device, "GtrPack", {
        FullscreenPass::UniformLayoutEntry(sUniformBinding, sizeof(PackUniforms)),
        FullscreenPass::TextureLayoutEntry(sColorBinding, WGPUTextureSampleType_Float),
        FullscreenPass::TextureLayoutEntry(sViewDepthBinding, WGPUTextureSampleType_UnfilterableFloat),
        FullscreenPass::TextureLayoutEntry(sCoverageBinding, WGPUTextureSampleType_UnfilterableFloat)
    });
    if (mShader == nullptr || mBindGroupLayout == nullptr)
        return false;
    mColorPipeline = FullscreenPass::CreatePipeline(gpu.Device, mShader, "GtrPackColor", mBindGroupLayout, "fs_color", WGPUTextureFormat_RGBA8Unorm);
    mDepthPipeline = FullscreenPass::CreatePipeline(gpu.Device, mShader, "GtrPackDepth", mBindGroupLayout, "fs_depth", WGPUTextureFormat_R32Float);
    if (mColorPipeline == nullptr || mDepthPipeline == nullptr)
        return false;

    const PackUniforms uniforms { mMetresPerStud, SceneView::EmptyViewDepth * 0.5f, GTR_FRAME_EMPTY_DEPTH, 0.0f };
    mUniformBuffer = gpu.CreateBuffer(sizeof(uniforms), WGPUBufferUsage_Uniform, &uniforms, "GtrPackUniforms");

    mView = new SceneView(gpu, mRenderer.GetSceneResources(), true);
    // Laid over the host's picture: no sky of its own behind it, and what its see-through parts cover kept
    mView->SetTransparentCoverage(true);
    mView->SetSkyDrawn(false);
    mLightView = new SceneView(gpu, mRenderer.GetSceneResources(), true);
    CreateReadableTarget(gpu.Device, "GtrLight", WGPUTextureFormat_R32Float, GTR_LIGHT_SIZE, GTR_LIGHT_SIZE, mLightTexture, mLightTextureView);
    std::thread([this]() { WriteQueued(); }).detach();
    return true;
}

void FrameExporter::CreateTargets(uint32_t width, uint32_t height) {
    FullscreenPass::ReleaseTarget(mColorTexture, mColorView);
    FullscreenPass::ReleaseTarget(mDepthTexture, mDepthView);
    CreateReadableTarget(mRenderer.Gpu.Device, "GtrColor", WGPUTextureFormat_RGBA8Unorm, width, height, mColorTexture, mColorView);
    CreateReadableTarget(mRenderer.Gpu.Device, "GtrDepth", WGPUTextureFormat_R32Float, width, height, mDepthTexture, mDepthView);
    mWidth = width;
    mHeight = height;
}

void FrameExporter::Render(Camera *camera, uint32_t width, uint32_t height, const HostPose &pose, const PixelRect &drawn,
                           Camera *light, const HostPose &lightPose) {
    if (mView == nullptr || camera == nullptr)
        return;
    width = std::clamp(width, sMinimumSize, GTR_FRAME_MAX_WIDTH);
    height = std::clamp(height, sMinimumSize, GTR_FRAME_MAX_HEIGHT);

    // With every readback still on its way back from the GPU, or being written out, the frame is skipped instead of waited for
    const auto free = std::ranges::find_if(mReadbacks, [](const Readback &readback) { return readback.State == ReadbackState::Free; });
    if (free == mReadbacks.end())
        return;
    Readback &readback = *free;

    GpuContext &gpu = mRenderer.Gpu;
    if (width != mWidth || height != mHeight)
        CreateTargets(width, height);
    mView->Resize(width, height);

    // The interface is drawn at the size of the guest's own window, which is the size its layout was worked out for
    GuiRenderer *gui = mRenderer.GetSceneResources().Gui;
    const uint32_t guiWidth = gui != nullptr ? std::min(gpu.Width, GTR_FRAME_MAX_WIDTH) : 0;
    const uint32_t guiHeight = gui != nullptr ? std::min(gpu.Height, GTR_FRAME_MAX_HEIGHT) : 0;
    const bool hasGui = guiWidth >= sMinimumSize && guiHeight >= sMinimumSize;
    if (hasGui && (guiWidth != mGuiWidth || guiHeight != mGuiHeight)) {
        FullscreenPass::ReleaseTarget(mGuiTexture, mGuiView);
        CreateReadableTarget(gpu.Device, "GtrGui", gpu.SurfaceFormat, guiWidth, guiHeight, mGuiTexture, mGuiView);
        mGuiWidth = guiWidth;
        mGuiHeight = guiHeight;
    }
    const uint32_t guiRowBytes = hasGui ? (guiWidth * sBytesPerPixel + sRowAlignment - 1) / sRowAlignment * sRowAlignment : 0;

    // The part to copy, kept inside the picture and never nothing
    PixelRect rect;
    rect.X = std::min(drawn.X, width - 1);
    rect.Y = std::min(drawn.Y, height - 1);
    rect.Width = std::clamp(drawn.Width, 1u, width - rect.X);
    rect.Height = std::clamp(drawn.Height, 1u, height - rect.Y);

    const uint32_t rowBytes = (rect.Width * sBytesPerPixel + sRowAlignment - 1) / sRowAlignment * sRowAlignment;
    const uint64_t layerBytes = (uint64_t)rowBytes * rect.Height;
    const uint64_t guiBytes = (uint64_t)guiRowBytes * guiHeight;
    // The shadow map's rows are a multiple of the alignment as they are
    const bool hasLight = light != nullptr && mLightView != nullptr;
    const uint64_t lightBytes = hasLight ? (uint64_t)GTR_LIGHT_SIZE * GTR_LIGHT_SIZE * sBytesPerPixel : 0;
    const uint64_t bufferBytes = layerBytes * 2 + guiBytes + lightBytes;
    if (hasLight)
        mLightView->Resize(GTR_LIGHT_SIZE, GTR_LIGHT_SIZE);
    if (readback.Capacity < bufferBytes) {
        if (readback.Buffer != nullptr)
            wgpuBufferRelease(readback.Buffer);
        WGPUBufferDescriptor bufferDesc {};
        bufferDesc.label = Gfx::Str("GtrReadback");
        bufferDesc.size = bufferBytes;
        bufferDesc.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        readback.Buffer = wgpuDeviceCreateBuffer(gpu.Device, &bufferDesc);
        readback.Capacity = bufferDesc.size;
    }

    WGPUCommandEncoderDescriptor encoderDesc {};
    encoderDesc.label = Gfx::Str("GtrExport");
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(gpu.Device, &encoderDesc);
    mView->Render(encoder, mView->GetColorView(), camera);
    // The coverage target is made as the view first draws
    if (mView->GetColorView() != mBoundColor || mView->GetViewDepthView() != mBoundViewDepth || mView->GetCoverageView() != mBoundCoverage) {
        FullscreenPass::ReleaseBindGroup(mBindGroup);
        mBoundColor = mView->GetColorView();
        mBoundViewDepth = mView->GetViewDepthView();
        mBoundCoverage = mView->GetCoverageView();
        mBindGroup = FullscreenPass::CreateBindGroup(gpu.Device, "GtrPack", mBindGroupLayout, {
            FullscreenPass::BufferEntry(sUniformBinding, mUniformBuffer, 0, sizeof(PackUniforms)),
            FullscreenPass::TextureEntry(sColorBinding, mBoundColor),
            FullscreenPass::TextureEntry(sViewDepthBinding, mBoundViewDepth),
            FullscreenPass::TextureEntry(sCoverageBinding, mBoundCoverage)
        });
        // The shadow map's packing reads the same layout; the frame's coverage stands in for one it has no use for
        FullscreenPass::ReleaseBindGroup(mLightBindGroup);
        mLightBoundColor = nullptr;
    }
    FullscreenPass::Run(encoder, "GtrPackColor", mColorPipeline, mBindGroup, mColorView);
    FullscreenPass::Run(encoder, "GtrPackDepth", mDepthPipeline, mBindGroup, mDepthView);
    CopyToBuffer(encoder, mColorTexture, readback.Buffer, 0, rowBytes, rect.Width, rect.Height, rect.X, rect.Y);
    CopyToBuffer(encoder, mDepthTexture, readback.Buffer, layerBytes, rowBytes, rect.Width, rect.Height, rect.X, rect.Y);
    if (hasGui) {
        // What the renderer last drew of the interface, drawn again over nothing: its colours come out multiplied by their
        // alpha, which is how the host lays them over its picture
        WGPURenderPassColorAttachment attachment {};
        attachment.view = mGuiView;
        attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        attachment.loadOp = WGPULoadOp_Clear;
        attachment.storeOp = WGPUStoreOp_Store;
        attachment.clearValue = { 0.0, 0.0, 0.0, 0.0 };
        WGPURenderPassDescriptor passDesc {};
        passDesc.label = Gfx::Str("GtrGui");
        passDesc.colorAttachmentCount = 1;
        passDesc.colorAttachments = &attachment;
        // The renderer has laid the interface out for this frame and not yet handed it to the GPU: drawn from what the GPU
        // still has of the last frame, the two don't go together, and the interface flickers whenever it changes
        gui->Upload();
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);
        gui->Render(pass);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        CopyToBuffer(encoder, mGuiTexture, readback.Buffer, layerBytes * 2, guiRowBytes, guiWidth, guiHeight);
    }
    if (hasLight) {
        mLightView->Render(encoder, mLightView->GetColorView(), light);
        if (mLightView->GetColorView() != mLightBoundColor || mLightView->GetViewDepthView() != mLightBoundViewDepth || mLightBindGroup == nullptr) {
            FullscreenPass::ReleaseBindGroup(mLightBindGroup);
            mLightBoundColor = mLightView->GetColorView();
            mLightBoundViewDepth = mLightView->GetViewDepthView();
            mLightBindGroup = FullscreenPass::CreateBindGroup(gpu.Device, "GtrPackLight", mBindGroupLayout, {
                FullscreenPass::BufferEntry(sUniformBinding, mUniformBuffer, 0, sizeof(PackUniforms)),
                FullscreenPass::TextureEntry(sColorBinding, mLightBoundColor),
                FullscreenPass::TextureEntry(sViewDepthBinding, mLightBoundViewDepth),
                FullscreenPass::TextureEntry(sCoverageBinding, mBoundCoverage)
            });
        }
        FullscreenPass::Run(encoder, "GtrPackLight", mDepthPipeline, mLightBindGroup, mLightTextureView);
        CopyToBuffer(encoder, mLightTexture, readback.Buffer, layerBytes * 2 + guiBytes, GTR_LIGHT_SIZE * sBytesPerPixel, GTR_LIGHT_SIZE, GTR_LIGHT_SIZE);
    }

    WGPUCommandBufferDescriptor commandsDesc {};
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, &commandsDesc);
    wgpuQueueSubmit(gpu.Queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);

    readback.Width = width;
    readback.Height = height;
    readback.RowBytes = rowBytes;
    readback.Rect = rect;
    readback.GuiWidth = hasGui ? guiWidth : 0;
    readback.GuiHeight = hasGui ? guiHeight : 0;
    readback.GuiRowBytes = guiRowBytes;
    readback.LightSize = hasLight ? GTR_LIGHT_SIZE : 0;
    readback.LightPose = lightPose;
    readback.Pose = pose;
    readback.GuestSeconds = SteadySeconds();
    readback.Order = mNextOrder++;
    readback.State = ReadbackState::Pending;

    WGPUBufferMapCallbackInfo callback {};
    callback.mode = WGPUCallbackMode_AllowProcessEvents;
    callback.userdata1 = &readback;
    callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *userdata1, void *) {
        static_cast<Readback *>(userdata1)->State = status == WGPUMapAsyncStatus_Success ? ReadbackState::Ready : ReadbackState::Failed;
    };
    wgpuBufferMapAsync(readback.Buffer, WGPUMapMode_Read, 0, bufferBytes, callback);
}

void FrameExporter::Publish() {
    if (mView == nullptr)
        return;
    wgpuDevicePoll(mRenderer.Gpu.Device, false, nullptr);

    // Oldest first, so the newest is the one left named in the header
    std::array<Readback *, GTR_FRAME_SLOTS> order;
    for (size_t i = 0; i < mReadbacks.size(); i++)
        order[i] = &mReadbacks[i];
    std::ranges::sort(order, {}, &Readback::Order);
    bool queued = false;
    for (Readback *readback : order) {
        const ReadbackState state = readback->State;
        if (state == ReadbackState::Failed) {
            readback->State = ReadbackState::Free;
        } else if (state == ReadbackState::Written) {
            wgpuBufferUnmap(readback->Buffer);
            readback->State = ReadbackState::Free;
        } else if (state == ReadbackState::Ready) {
            const uint64_t bytes = (uint64_t)readback->RowBytes * readback->Rect.Height * 2 + (uint64_t)readback->GuiRowBytes * readback->GuiHeight +
                                   (uint64_t)readback->LightSize * readback->LightSize * sBytesPerPixel;
            readback->Pixels = static_cast<const uint8_t *>(wgpuBufferGetConstMappedRange(readback->Buffer, 0, bytes));
            if (readback->Pixels == nullptr) {
                wgpuBufferUnmap(readback->Buffer);
                readback->State = ReadbackState::Free;
                continue;
            }
            readback->State = ReadbackState::Writing;
            std::lock_guard lock(mQueueMutex);
            mQueue.push_back(readback);
            queued = true;
        }
    }
    if (queued)
        mQueueReady.notify_one();
}

void FrameExporter::WriteQueued() {
    for (;;) {
        Readback *readback;
        {
            std::unique_lock lock(mQueueMutex);
            mQueueReady.wait(lock, [this]() { return !mQueue.empty(); });
            readback = mQueue.front();
            mQueue.pop_front();
        }
        Write(*readback, readback->Pixels);
        readback->State = ReadbackState::Written;
    }
}

void FrameExporter::Write(const Readback &readback, const uint8_t *pixels) {
    const int32_t slotIndex = (mHeader->LatestSlot + 1) % GTR_FRAME_SLOTS;
    GtrFrameSlot &slot = mHeader->Slots[slotIndex];
    slot.Sequence++;
    std::atomic_thread_fence(std::memory_order_seq_cst);

    slot.CameraId = readback.Pose.CameraId;
    slot.Width = readback.Width;
    slot.Height = readback.Height;
    slot.FovY = readback.Pose.FovY;
    memcpy(slot.CameraPosition, readback.Pose.Position, sizeof(slot.CameraPosition));
    memcpy(slot.CameraRight, readback.Pose.Right, sizeof(slot.CameraRight));
    memcpy(slot.CameraForward, readback.Pose.Forward, sizeof(slot.CameraForward));
    memcpy(slot.CameraUp, readback.Pose.Up, sizeof(slot.CameraUp));
    slot.GuestSeconds = readback.GuestSeconds;
    slot.GuiWidth = readback.GuiWidth;
    slot.GuiHeight = readback.GuiHeight;
    slot.LightSize = readback.LightSize;
    slot.LightTan = std::tan(readback.LightPose.FovY * 3.14159265f / 360.0f);
    memcpy(slot.LightPosition, readback.LightPose.Position, sizeof(slot.LightPosition));
    memcpy(slot.LightRight, readback.LightPose.Right, sizeof(slot.LightRight));
    memcpy(slot.LightForward, readback.LightPose.Forward, sizeof(slot.LightForward));
    memcpy(slot.LightUp, readback.LightPose.Up, sizeof(slot.LightUp));
    const WGPUTextureFormat guiFormat = mRenderer.Gpu.SurfaceFormat;
    slot.GuiBlueFirst = guiFormat == WGPUTextureFormat_BGRA8Unorm || guiFormat == WGPUTextureFormat_BGRA8UnormSrgb ? 1 : 0;

    const PixelRect &rect = readback.Rect;
    slot.RectX = (uint16_t)rect.X;
    slot.RectY = (uint16_t)rect.Y;
    slot.RectWidth = (uint16_t)rect.Width;
    slot.RectHeight = (uint16_t)rect.Height;

    // Each layer keeps the whole picture's layout, and the copied part is written into its place in it
    const size_t pictureRowBytes = (size_t)readback.Width * sBytesPerPixel;
    const size_t pictureBytes = pictureRowBytes * readback.Height;
    const size_t rectRowBytes = (size_t)rect.Width * sBytesPerPixel;
    const size_t sourceLayerBytes = (size_t)readback.RowBytes * rect.Height;
    uint8_t *out = reinterpret_cast<uint8_t *>(mHeader) + GTR_FRAME_HEADER_BYTES + mHeader->SlotStride * slotIndex;
    for (const size_t layer : { (size_t)0, (size_t)1 }) {
        uint8_t *to = out + pictureBytes * layer + pictureRowBytes * rect.Y + (size_t)rect.X * sBytesPerPixel;
        const uint8_t *from = pixels + sourceLayerBytes * layer;
        for (uint32_t row = 0; row < rect.Height; row++, to += pictureRowBytes, from += readback.RowBytes)
            memcpy(to, from, rectRowBytes);
    }
    out += pictureBytes * 2;

    const size_t guiTightRowBytes = (size_t)readback.GuiWidth * sBytesPerPixel;
    const size_t guiBytes = guiTightRowBytes * readback.GuiHeight;
    const uint8_t *guiPixels = pixels + sourceLayerBytes * 2;
    bool sameGui = readback.GuiWidth == mGuiPictureWidth && readback.GuiHeight == mGuiPictureHeight && mGui.size() == guiBytes && mGuiVersion != 0;
    for (uint32_t row = 0; sameGui && row < readback.GuiHeight; row++)
        sameGui = memcmp(mGui.data() + guiTightRowBytes * row, guiPixels + (size_t)readback.GuiRowBytes * row, guiTightRowBytes) == 0;
    if (!sameGui) {
        mGui.resize(guiBytes);
        for (uint32_t row = 0; row < readback.GuiHeight; row++)
            memcpy(mGui.data() + guiTightRowBytes * row, guiPixels + (size_t)readback.GuiRowBytes * row, guiTightRowBytes);
        mGuiPictureWidth = readback.GuiWidth;
        mGuiPictureHeight = readback.GuiHeight;
        // Counted on from the clock, so that a guest started again doesn't reuse the numbers of the one before
        mGuiVersion = std::max(mGuiVersion + 1, (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count());
    }
    slot.GuiVersion = mGuiVersion;
    const size_t guiOffset = pictureBytes * 2;
    if (mSlotGui[slotIndex] != std::pair(mGuiVersion, guiOffset)) {
        memcpy(out, mGui.data(), guiBytes);
        mSlotGui[slotIndex] = { mGuiVersion, guiOffset };
    }
    out += guiBytes;
    if (readback.LightSize != 0)
        memcpy(out, guiPixels + (size_t)readback.GuiRowBytes * readback.GuiHeight, (size_t)readback.LightSize * readback.LightSize * sBytesPerPixel);

    std::atomic_thread_fence(std::memory_order_seq_cst);
    slot.Sequence++;
    mHeader->LatestSlot = slotIndex;
    mHeader->Published++;
    if (mPublishedEvent != nullptr)
        SetEvent(mPublishedEvent);
}

int64_t FrameExporter::GetPublishedCount() const {
    return mHeader != nullptr ? mHeader->Published : 0;
}
