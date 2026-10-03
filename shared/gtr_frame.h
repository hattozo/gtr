// The frames the guest (Vanadium) publishes for the host (GTA V) to composite, as both sides and host/gtrframe.py read them.
//
// A named, pagefile-backed mapping holds a header and a ring of slots. The guest writes a slot (its Sequence is odd meanwhile), then
// names it in the header and bumps Published. A reader copies the newest slot and checks that Sequence is even and did not change.
//
// Every position and direction here is in host space: GTA's world, metres, X east, Y north, Z up. The guest converts to and from
// its own studs, so the host never needs to know the guest's units.
#pragma once

#include <stdint.h>

#define GTR_FRAME_MAPPING_NAME "Local\\GtrPassthroughFrame"
#define GTR_FRAME_MAGIC 0x46525447u // "GTRF"
#define GTR_FRAME_VERSION 5u
// Enough that a frame the host chose to show is still there when it gets to showing it, a frame or two of its own later,
// with the guest publishing several times as fast
#define GTR_FRAME_SLOTS 8
#define GTR_FRAME_HEADER_BYTES 4096
// Larger pictures are drawn at this size and stretched by the host
#define GTR_FRAME_MAX_WIDTH 2560u
#define GTR_FRAME_MAX_HEIGHT 1440u
// The guest's shadow map is this many pixels each way
#define GTR_LIGHT_SIZE 512u
// A pixel's depth where the guest drew nothing
#define GTR_FRAME_EMPTY_DEPTH 1.0e9f

// The link the host drives the guest over: one JSON object per line, TCP on 127.0.0.1 only
#define GTR_LINK_PORT 25610

#pragma pack(push, 1)
typedef struct GtrFrameSlot {
    int64_t Sequence;      // odd while the guest is writing this slot
    // Which camera this frame was drawn from: the "id" of the host's cam message, or, when the guest's own camera is the
    // one in charge, a number the guest counts up. The pose below is that camera's either way.
    int64_t CameraId;
    uint32_t Width;
    uint32_t Height;
    float FovY;            // vertical, degrees
    float Reserved0;
    double CameraPosition[3];
    float CameraRight[3];
    float CameraForward[3];
    float CameraUp[3];
    uint32_t Reserved1;
    double GuestSeconds;   // the guest's steady clock when it drew the frame
    // The guest's 2D interface (its place's GUIs and its pointer), a picture of its own that goes over everything: its size,
    // 0 by 0 when there is none, and whether its bytes are blue first (BGRA) or red first
    uint32_t GuiWidth;
    uint32_t GuiHeight;
    uint32_t GuiBlueFirst;
    // The part of the picture the guest drew anything in, in pixels from the top left: only that part of the colour and the
    // depth is written, in its place in the whole, and the rest is to be taken as empty whatever bytes are there. Copying a
    // whole picture out of the GPU each frame is what the guest's time went on, and most of a picture is empty.
    uint16_t RectX;
    uint16_t RectY;
    uint16_t RectWidth;
    uint16_t RectHeight;
    uint8_t Padding0[4];
    // The guest seen from the host's sun, for the shadows it casts on the host's world: a depth picture, LightSize pixels
    // each way (0 when there is none), drawn from a camera far off towards the sun and looking at the guest's character.
    // That camera, as the frame's own is given above, and the tangent of half its field of view
    double LightPosition[3];
    float LightRight[3];
    float LightForward[3];
    float LightUp[3];
    float LightTan;
    uint32_t LightSize;
    uint8_t Padding[60];
} GtrFrameSlot;

typedef struct GtrFrameHeader {
    uint32_t Magic;
    uint32_t Version;
    int32_t SlotCount;
    uint32_t MaxWidth;
    uint32_t MaxHeight;
    uint32_t Reserved;
    // Bytes from one slot's pixels to the next: MaxWidth * MaxHeight * 12 + GTR_LIGHT_SIZE * GTR_LIGHT_SIZE * 4
    int64_t SlotStride;
    int64_t Published;     // counts frames; 0 until the first
    int32_t LatestSlot;
    int32_t Reserved2;
    uint8_t Padding[208];
    GtrFrameSlot Slots[GTR_FRAME_SLOTS];
} GtrFrameHeader;

// What the host's script tells the host's compositor, in a mapping of its own: the two are separate modules in one process (a
// ScriptHookV script and a ReShade add-on). Without it the compositor uses the effect's own settings.
#define GTR_HOST_MAPPING_NAME "Local\\GtrPassthroughHost"
#define GTR_HOST_MAGIC 0x48525447u // "GTRH"
// How many ticks back the compositor can still find the frame for; a mark has one colour bit a channel, so 8
#define GTR_HOST_TICKS 8
// The mark: two squares side by side in the picture's top left corner, this many pixels each way. The first has a channel
// full on for each set bit of the tick's number and the second is its opposite, which is how a picture without a mark is told
// from one with it.
#define GTR_MARK_PIXELS 8

typedef struct GtrHostState {
    uint32_t Magic;
    uint32_t Version;
    int32_t Active;          // 0 hides the guest: menus, cutscenes, the passthrough switched off
    float NearClip;          // the host camera's clip planes, which turn its depth buffer into metres
    float FarClip;
    // Each change saves the finished picture as a 32-bit BMP at CapturePath; CaptureDone then takes the same value
    int32_t CaptureRequest;
    int32_t CaptureDone;
    // Counts the script's ticks. The game stops its scripts in the pause menu without a word, and a count that has stopped is
    // how the compositor knows to hide the guest then
    uint32_t Heartbeat;
    char CapturePath[260];
    // When the script puts the host's camera where a guest frame was drawn from, the two pictures must be shown together,
    // and the script can't know which of its ticks a picture being finished belongs to: the game draws on another thread,
    // a tick or more behind. So each tick the script draws its number (the low bits) into the picture as a mark, and writes
    // here the CameraId of the frame it chose that tick. The compositor reads the mark off the picture and shows that
    // frame. All zero when the script doesn't do this; the newest frame is shown then.
    int64_t TickCamera[GTR_HOST_TICKS];
    // Added to the mark's number before it is looked up, should the game ever draw the mark a tick apart from the camera
    int32_t MarkOffset;
    // For the tools that check a running session (host/calibrate.py), which write these themselves: DebugView, when not 0,
    // is one more than the effect's debug view to show, and ShowMark leaves the mark in the finished picture
    int32_t DebugView;
    int32_t ShowMark;
    // The host's sun or moon, for the shadows the guest casts on the host's world: the direction towards it in the
    // camera's space (right, up, forward), and how much darker its shadows are than its light (0 none, 1 black)
    float SunView[3];
    float SunShadow;
    // The tangent of half the camera's vertical field of view
    float TanHalfFov;
    // The world's up in the camera's space (right, up, forward): how steeply the camera looks down
    float UpView[3];
    // What the sun's shadow takes from red, green and blue, for each the share of the darkness SunShadow says: the sun's
    // own color, which a shadow is left without. All 0: the effect's own
    float SunTint[3];
    uint8_t Padding[220 - 8 * GTR_HOST_TICKS - 56];
} GtrHostState;
#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(GtrHostState) == 512, "GtrHostState layout");
static_assert(sizeof(GtrFrameSlot) == 256, "GtrFrameSlot layout");
static_assert(sizeof(GtrFrameHeader) == 256 + 256 * GTR_FRAME_SLOTS, "GtrFrameHeader layout");
static_assert(sizeof(GtrFrameHeader) <= GTR_FRAME_HEADER_BYTES, "GtrFrameHeader fits its page");
#endif

// A slot's pixels start at GTR_FRAME_HEADER_BYTES + SlotStride * slot:
//   Width * Height * 4 bytes  colour, RGBA8, rows top to bottom, display-referred; alpha is 255 where the guest drew a part and 0 elsewhere
//   Width * Height * 4 bytes  depth, float32: metres along the camera's forward axis (not along the pixel's ray), or
//                             GTR_FRAME_EMPTY_DEPTH
//   GuiWidth * GuiHeight * 4  the interface, rows top to bottom, colour already multiplied by its alpha
//   LightSize * LightSize * 4 the shadow map, float32: metres along the light camera's forward axis, or GTR_FRAME_EMPTY_DEPTH
