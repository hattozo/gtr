// Composites Vanadium (the passthrough guest) into the host's picture. The GtrCompositor add-on uploads the guest's frames
// into GTRCOLOR0.. and GTRDEPTH0.. and sets the uniforms marked "set by the add-on".
//
// colour: RGBA, alpha 1 where the guest drew a part. depth: metres along the camera's forward axis, which is what a depth
// buffer linearises to, so the two compare directly.
//
// The guest is lit by its own renderer, as at a plain afternoon. To sit in the host's picture it is given the host's light:
// the brightness and colour of the host's picture around each pixel, and the colour of the picture as a whole, which is how
// the host's own grading treats its world. Its edges are softened, since the host's anti-aliasing ran before it was added,
// and the host's surfaces are darkened where the guest stands on them.
#include "ReShade.fxh"

// Eight guest frames are kept ready. Without a host script that marks its pictures only the first is used, and holds the
// newest frame. With one, each holds the frame the script chose at one of its last eight ticks, when it put the host's camera
// where that frame was drawn from; the picture carries the tick's number as a mark (see GTR_MARK_PIXELS in gtr_frame.h), and
// the frame sampled is the one for that tick. So the two pictures are always of the same camera, however many ticks the
// host takes to finish a picture.
#define GTR_LAYER(n) \
	uniform float4 GtrLayerRect##n = float4(0.0, 0.0, 1.0, 1.0); \
	uniform float4 GtrSunRowA##n = float4(1.0, 0.0, 0.0, 0.0); \
	uniform float4 GtrSunRowB##n = float4(0.0, 1.0, 0.0, 0.0); \
	uniform float4 GtrSunRowC##n = float4(0.0, 0.0, 1.0, 0.0); \
	uniform float GtrSunTan##n = 0.0; \
	texture GtrSunMapTex##n : GTRSUNMAP##n; \
	sampler sGtrSunMap##n { Texture = GtrSunMapTex##n; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; }; \
	texture GtrColorTex##n : GTRCOLOR##n; \
	texture GtrDepthTex##n : GTRDEPTH##n; \
	sampler sGtrColor##n { Texture = GtrColorTex##n; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; }; \
	sampler sGtrDepth##n { Texture = GtrDepthTex##n; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
GTR_LAYER(0)
GTR_LAYER(1)
GTR_LAYER(2)
GTR_LAYER(3)
GTR_LAYER(4)
GTR_LAYER(5)
GTR_LAYER(6)
GTR_LAYER(7)
// The guest's interface: its place's GUIs and its pointer, to go over the whole picture
texture GtrGuiTex : GTRGUI;
sampler sGtrGui { Texture = GtrGuiTex; AddressU = CLAMP; AddressV = CLAMP; };
#define GTR_MARK_PIXELS 8
// The depth kept for a pixel the guest drew nothing in: far, and within what a half float holds
#define GTR_NO_GUEST 60000.0
#define GTR_SHADOW_STEPS 24
#define GTR_SUN_MAP_SIZE 512.0
#define GTR_CONTACT_TAPS 16

// Set by the add-on, true only while guest frames are arriving; until then the host's picture passes through untouched.
// (The technique stays enabled and this gates it: toggling a technique from inside an effect callback crashes ReShade.)
uniform bool GtrActive = false;
// Set by the add-on: whether the host's pictures are marked, what to add to a mark's number, and the layer of the newest
// tick, for a picture whose mark can't be read
uniform bool GtrMarked = false;
uniform int GtrMarkOffset = 0;
uniform int GtrLatestLayer = 0;
// Set by the add-on: the layer that holds the frame of each of the script's ticks, the first four and the last four
uniform float4 GtrTickLayerA = float4(0, 0, 0, 0);
uniform float4 GtrTickLayerB = float4(0, 0, 0, 0);
// Set by the add-on for a tool that wants to see the mark in the finished picture
uniform bool GtrShowMark = false;
// Set by the add-on while the guest's frames carry an interface
uniform bool GtrGui = false;
// Set by the add-on from the host's script: the direction towards the host's sun or moon in the camera's space (x right,
// y up, z forward), and how much darker its shadows are than its light; 0 while the script knows of no sun
uniform float3 GtrSunView = float3(0.0, 1.0, 0.0);
uniform float GtrSunShadow = 0.0;
// The world's up in the camera's space, likewise
uniform float3 GtrUpView = float3(0.0, 1.0, 0.0);
// What the sun's shadow takes from each color, from the host by the hour (see sun_tint)
uniform float3 GtrSunTint = float3(1.05, 1.0, 0.92);
// Set by the add-on when the host's script publishes its camera's clip planes; otherwise these are used
uniform float2 HostPlanes < ui_type = "drag"; ui_min = 0.01; ui_max = 20000.0; ui_label = "Host near and far clip (m)"; > = float2(0.15, 10000.0);

uniform bool HostReversedZ < ui_label = "Host depth is reversed"; > = true;
uniform float HostDepthScale < ui_type = "drag"; ui_min = 0.5; ui_max = 2.0; ui_step = 0.001; ui_label = "Host depth scale";
	ui_tooltip = "Calibration: multiplies the host's linearised depth."; > = 1.0;
uniform float DepthBias < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.005; ui_label = "Depth bias (m)";
	ui_tooltip = "How far behind the host's surface the guest may still show (bricks resting on the ground)."; > = 0.02;

uniform float LightMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match the host's lighting";
	ui_tooltip = "Relight the guest by the (blurred) host picture around it: darker in shade and at night, tinted by nearby light."; > = 0.85;
uniform float LightReference < ui_type = "drag"; ui_min = 0.1; ui_max = 1.0; ui_step = 0.01; ui_label = "Neutral brightness";
	ui_tooltip = "The host brightness at which the guest keeps its own colours."; > = 0.42;
uniform float LightTint < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Light colour";
	ui_tooltip = "How much of the colour of the host's picture around it the guest takes on. Much of it and a white character on red tiles turns pink."; > = 0.25;
uniform float LightBlur < ui_type = "drag"; ui_min = 1.0; ui_max = 6.0; ui_step = 0.1; ui_label = "Light blur (mip)";
	ui_tooltip = "How wide the host's picture is looked at for the light on the guest: wide enough that it is the light that is read and not the ground's own colour."; > = 4.6;
uniform float2 LightRange < ui_type = "drag"; ui_min = 0.05; ui_max = 2.0; ui_step = 0.01; ui_label = "Darkest and brightest relight"; > = float2(0.22, 1.35);
uniform float GradeMatch < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Match the host's colour grade";
	ui_tooltip = "Give the guest the colour of the host's whole picture (an orange sunset, a blue night)."; > = 0.5;
uniform float HazeStart < ui_type = "drag"; ui_min = 0.0; ui_max = 200.0; ui_step = 1.0; ui_label = "Haze start (m)"; > = 25.0;
uniform float HazeDistance < ui_type = "drag"; ui_min = 10.0; ui_max = 1000.0; ui_step = 1.0; ui_label = "Haze distance (m)";
	ui_tooltip = "Far away the guest fades into the host picture around it, as the host's haze and fog do to its own world."; > = 220.0;
uniform float HazeStrength < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Haze strength"; > = 0.8;
uniform float EdgeSoftness < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Soften the guest's edges"; > = 1.0;
uniform float ContactShadow < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Contact shadows";
	ui_tooltip = "Darken the host's surfaces right next to the guest's (under a character's feet, around bricks)."; > = 0.75;
uniform float ContactRadius < ui_type = "drag"; ui_min = 0.1; ui_max = 3.0; ui_step = 0.05; ui_label = "Contact shadow size (m)"; > = 0.55;
uniform float ContactGain < ui_type = "drag"; ui_min = 0.5; ui_max = 12.0; ui_step = 0.1; ui_label = "Contact shadow gain";
	ui_tooltip = "How quickly the contact shadow reaches its full darkness as the guest gets nearer."; > = 8.0;
uniform float SunShadows < ui_type = "drag"; ui_min = 0.0; ui_max = 1.5; ui_step = 0.01; ui_label = "Sun and moon shadows";
	ui_tooltip = "The guest's shadow on the host's world, away from the host's sun or moon. 1 is as dark as the host's own."; > = 1.0;
uniform float ShadowReach < ui_type = "drag"; ui_min = 1.0; ui_max = 30.0; ui_step = 0.5; ui_label = "Shadow reach (m)";
	ui_tooltip = "How far from a surface the guest is still looked for towards the sun."; > = 9.0;
uniform float ShadowThickness < ui_type = "drag"; ui_min = 0.1; ui_max = 3.0; ui_step = 0.05; ui_label = "Shadow caster thickness (m)";
	ui_tooltip = "The guest is known only by the side the camera sees: this is how deep behind it still counts as inside, for a camera that looks level."; > = 0.7;
uniform float ShadowThicknessDown < ui_type = "drag"; ui_min = 0.1; ui_max = 3.0; ui_step = 0.05; ui_label = "Shadow caster thickness from above (m)";
	ui_tooltip = "The same for a camera that looks straight down, which sees a standing character from its head to its feet."; > = 1.9;
uniform float SunMapBias < ui_type = "drag"; ui_min = 0.0; ui_max = 0.5; ui_step = 0.005; ui_label = "Shadow map bias (m)";
	ui_tooltip = "How far behind the guest, from the sun, a surface has to be to be in its shadow."; > = 0.09;
uniform float SunMapSoftness < ui_type = "drag"; ui_min = 0.5; ui_max = 4.0; ui_step = 0.05; ui_label = "Shadow map softness (texels)"; > = 1.0;
uniform float LampShadows < ui_type = "drag"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01; ui_label = "Lamp shadows";
	ui_tooltip = "The guest's shadow away from the brightest light in the host's picture, when the sun's is weak: at night, indoors. Off: it is a guess at one light from the picture, and unsteady."; > = 0.0;
uniform float LampBrightness < ui_type = "drag"; ui_min = 0.1; ui_max = 0.95; ui_step = 0.01; ui_label = "Lamp brightness";
	ui_tooltip = "How bright a thing in the host's picture has to be, at the least, to count towards where the lamp is."; > = 0.2;
uniform float LampContrast < ui_type = "drag"; ui_min = 1.0; ui_max = 6.0; ui_step = 0.05; ui_label = "Lamp contrast";
	ui_tooltip = "And how many times as bright as the picture as a whole."; > = 2.2;
uniform float LampCertainty < ui_type = "drag"; ui_min = 10.0; ui_max = 5000.0; ui_step = 10.0; ui_label = "Lamp certainty";
	ui_tooltip = "How quickly a little bright in the picture is believed to be a lamp."; > = 800.0;
uniform float LampRange < ui_type = "drag"; ui_min = 2.0; ui_max = 60.0; ui_step = 0.5; ui_label = "Lamp range (m)"; > = 14.0;
uniform float LampLift < ui_type = "drag"; ui_min = 0.0; ui_max = 5.0; ui_step = 0.05; ui_label = "Lamp lift (m)";
	ui_tooltip = "The light is taken to be this far above the bright thing seen, when that is a lamp's glass or a wall it lights."; > = 0.3;
uniform float LampLiftGround < ui_type = "drag"; ui_min = 0.0; ui_max = 12.0; ui_step = 0.1; ui_label = "Lamp lift over ground (m)";
	ui_tooltip = "And this far above it when it is ground: a street lamp is on a pole."; > = 4.5;
uniform float LampFarthest < ui_type = "drag"; ui_min = 5.0; ui_max = 300.0; ui_step = 1.0; ui_label = "Farthest lamp (m)"; > = 40.0;
uniform float LampSunLimit < ui_type = "drag"; ui_min = 0.05; ui_max = 1.0; ui_step = 0.01; ui_label = "Sun shadow at which lamps stop counting"; > = 0.3;
// The tangent of half the camera's vertical field of view, which turns metres into pixels; set by the add-on when the host's
// script publishes it
uniform float ViewTangent < ui_type = "drag"; ui_min = 0.1; ui_max = 2.0; ui_step = 0.01; ui_label = "tan(vertical FOV / 2)"; > = 0.7;
uniform int DebugView < ui_type = "combo"; ui_items = "Composite\0Host depth (1 m bands)\0Guest depth (1 m bands)\0Depth difference\0Guest as drawn\0Shadows (sun red, contact green, lamp blue)\0"; > = 0;

// The host's picture at quarter size with mips: its blurred levels stand in for the light around each pixel
texture GtrLightTex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA8; MipLevels = 7; };
sampler sGtrLight { Texture = GtrLightTex; AddressU = CLAMP; AddressV = CLAMP; };
// The composited picture and, per pixel: whether the guest shows, the guest's depth (shown or not) and the host's
texture GtrCompositeTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler sGtrComposite { Texture = GtrCompositeTex; AddressU = CLAMP; AddressV = CLAMP; };
texture GtrInfoTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
sampler sGtrInfo { Texture = GtrInfoTex; MinFilter = POINT; MagFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };
// How much of the host's sun the guest keeps off each of the host's pixels, how closely the guest stands over them, and how
// much of the host's lamp it keeps off them
texture GtrShadowTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
// The bright things in the host's picture, weighed, for finding the lamp among them; its smallest mip is the whole picture's sum
#define GTR_LAMP_TOP_MIP 6
texture GtrLampTex { Width = 64; Height = 64; Format = RGBA16F; MipLevels = 7; };
sampler sGtrLamp { Texture = GtrLampTex; AddressU = CLAMP; AddressV = CLAMP; };
sampler sGtrShadow { Texture = GtrShadowTex; AddressU = CLAMP; AddressV = CLAMP; };

float luma(float3 c)
{
	return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float host_linear(float d)
{
	const float n = HostPlanes.x, f = HostPlanes.y;
	float z;
	if (HostReversedZ)
		z = d > 0.0 ? n * f / (n + d * (f - n)) : 1e9;
	else
		z = d < 1.0 ? n * f / (f - d * (f - n)) : 1e9;
	return z * HostDepthScale;
}

float3 bands(float z)
{
	return lerp(float3(0.1, 0.1, 0.1), float3(1.0, 0.85, 0.3), step(0.5, frac(z))) * saturate(1.5 - z / 100.0);
}

/// Which layer holds the guest frame that belongs to this picture. The mark is two squares in the top left corner: the first
/// has a channel full on for each set bit of the tick's number, the second is its opposite.
int guest_layer()
{
	if (!GtrMarked)
		return 0;
	const float2 centre = float2(0.5, 0.5) * GTR_MARK_PIXELS * BUFFER_PIXEL_SIZE;
	const float3 mark = tex2Dlod(ReShade::BackBuffer, float4(centre, 0, 0)).rgb;
	const float3 opposite = tex2Dlod(ReShade::BackBuffer, float4(centre + float2(GTR_MARK_PIXELS * BUFFER_RCP_WIDTH, 0), 0, 0)).rgb;
	if (any(abs(mark + opposite - 1.0) > 0.2) || any(abs(mark - 0.5) < 0.3))
		return GtrLatestLayer;
	const int tick = (mark.r > 0.5 ? 1 : 0) + (mark.g > 0.5 ? 2 : 0) + (mark.b > 0.5 ? 4 : 0);
	const int number = (tick + GtrMarkOffset + 8) % 8;
	const float4 layers = number < 4 ? GtrTickLayerA : GtrTickLayerB;
	return int(layers[number % 4] + 0.5);
}

/// The part of a layer's picture the guest drew anything in (left, top, right, bottom, as fractions): the add-on copies only
/// that much of each frame, so outside it a layer still has an earlier frame's pixels, and they are taken for empty.
float4 layer_rect(int layer)
{
	if (layer == 0) return GtrLayerRect0;
	if (layer == 1) return GtrLayerRect1;
	if (layer == 2) return GtrLayerRect2;
	if (layer == 3) return GtrLayerRect3;
	if (layer == 4) return GtrLayerRect4;
	if (layer == 5) return GtrLayerRect5;
	if (layer == 6) return GtrLayerRect6;
	return GtrLayerRect7;
}

bool in_layer(int layer, float2 uv)
{
	const float4 rect = layer_rect(layer);
	return uv.x >= rect.x && uv.y >= rect.y && uv.x < rect.z && uv.y < rect.w;
}

/// A layer's shadow map: the guest's depth as the host's sun sees it, in metres from a camera far off towards the sun.
float sun_map(int layer, float2 uv)
{
	const float4 at = float4(uv, 0, 0);
	if (layer == 0) return tex2Dlod(sGtrSunMap0, at).r;
	if (layer == 1) return tex2Dlod(sGtrSunMap1, at).r;
	if (layer == 2) return tex2Dlod(sGtrSunMap2, at).r;
	if (layer == 3) return tex2Dlod(sGtrSunMap3, at).r;
	if (layer == 4) return tex2Dlod(sGtrSunMap4, at).r;
	if (layer == 5) return tex2Dlod(sGtrSunMap5, at).r;
	if (layer == 6) return tex2Dlod(sGtrSunMap6, at).r;
	return tex2Dlod(sGtrSunMap7, at).r;
}

/// How much of the sun the guest keeps off a place, given in the camera's space, by the layer's shadow map: 0 to 1, or -1
/// when the layer has no map. The map is the guest's real outline from where the sun is, so the shadow is the guest's own
/// shape whatever the camera sees of it, and is there for what the camera doesn't see at all.
float sun_from_map(int layer, float3 at)
{
	float4 a = GtrSunRowA7, b = GtrSunRowB7, c = GtrSunRowC7;
	float tangent = GtrSunTan7;
	if (layer == 0) { a = GtrSunRowA0; b = GtrSunRowB0; c = GtrSunRowC0; tangent = GtrSunTan0; }
	else if (layer == 1) { a = GtrSunRowA1; b = GtrSunRowB1; c = GtrSunRowC1; tangent = GtrSunTan1; }
	else if (layer == 2) { a = GtrSunRowA2; b = GtrSunRowB2; c = GtrSunRowC2; tangent = GtrSunTan2; }
	else if (layer == 3) { a = GtrSunRowA3; b = GtrSunRowB3; c = GtrSunRowC3; tangent = GtrSunTan3; }
	else if (layer == 4) { a = GtrSunRowA4; b = GtrSunRowB4; c = GtrSunRowC4; tangent = GtrSunTan4; }
	else if (layer == 5) { a = GtrSunRowA5; b = GtrSunRowB5; c = GtrSunRowC5; tangent = GtrSunTan5; }
	else if (layer == 6) { a = GtrSunRowA6; b = GtrSunRowB6; c = GtrSunRowC6; tangent = GtrSunTan6; }
	if (tangent <= 0.0)
		return -1.0;
	const float3 seen = float3(dot(a.xyz, at) + a.w, dot(b.xyz, at) + b.w, dot(c.xyz, at) + c.w);
	if (seen.z <= 1.0)
		return 0.0;
	const float2 uv = float2(seen.x, -seen.y) / (seen.z * tangent) * 0.5 + 0.5;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;
	// A tent over the texels round the place, each compared and the results blended as a texture's are: the edge comes out
	// as smooth as the map is fine, without steps a texel wide
	const float2 texel = uv * GTR_SUN_MAP_SIZE / SunMapSoftness - 0.5;
	const float2 corner = floor(texel), part = texel - corner;
	float shade = 0.0;
	[unroll] for (int y = -1; y <= 2; ++y)
	{
		[unroll] for (int x = -1; x <= 2; ++x)
		{
			const float2 weight = float2(x == -1 ? 1.0 - part.x : x == 2 ? part.x : 1.0, y == -1 ? 1.0 - part.y : y == 2 ? part.y : 1.0);
			const float2 at = (corner + float2(x, y) + 0.5) * SunMapSoftness / GTR_SUN_MAP_SIZE;
			shade += weight.x * weight.y * (sun_map(layer, at) < seen.z - SunMapBias ? 1.0 : 0.0);
		}
	}
	return shade / 9.0;
}

float4 guest_color(int layer, float2 uv)
{
	if (!in_layer(layer, uv))
		return 0.0;
	const float4 at = float4(uv, 0, 0);
	if (layer == 0) return tex2Dlod(sGtrColor0, at);
	if (layer == 1) return tex2Dlod(sGtrColor1, at);
	if (layer == 2) return tex2Dlod(sGtrColor2, at);
	if (layer == 3) return tex2Dlod(sGtrColor3, at);
	if (layer == 4) return tex2Dlod(sGtrColor4, at);
	if (layer == 5) return tex2Dlod(sGtrColor5, at);
	if (layer == 6) return tex2Dlod(sGtrColor6, at);
	return tex2Dlod(sGtrColor7, at);
}

float guest_depth(int layer, float2 uv)
{
	if (!in_layer(layer, uv))
		return 1e9;
	const float4 at = float4(uv, 0, 0);
	if (layer == 0) return tex2Dlod(sGtrDepth0, at).r;
	if (layer == 1) return tex2Dlod(sGtrDepth1, at).r;
	if (layer == 2) return tex2Dlod(sGtrDepth2, at).r;
	if (layer == 3) return tex2Dlod(sGtrDepth3, at).r;
	if (layer == 4) return tex2Dlod(sGtrDepth4, at).r;
	if (layer == 5) return tex2Dlod(sGtrDepth5, at).r;
	if (layer == 6) return tex2Dlod(sGtrDepth6, at).r;
	return tex2Dlod(sGtrDepth7, at).r;
}

/// Where a pixel's surface is in the camera's space (x right, y up, z forward), from its depth along the forward axis.
float3 view_position(float2 uv, float z, float2 tangent)
{
	return float3((uv.x * 2.0 - 1.0) * tangent.x * z, (1.0 - uv.y * 2.0) * tangent.y * z, z);
}

float4 PS_Light(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	return float4(tex2D(ReShade::BackBuffer, uv).rgb, 1.0);
}

float3 relight(float3 c, float2 uv, float z)
{
	const float brightness = luma(tex2Dlod(sGtrLight, float4(uv, 0, LightBlur)).rgb);
	const float gain = clamp(brightness / LightReference, LightRange.x, LightRange.y);
	// The colour from wider still: nearby it is the colour of what the guest stands on, not of the light
	const float3 wide = tex2Dlod(sGtrLight, float4(uv, 0, LightBlur + 1.0)).rgb;
	const float3 tint = clamp(lerp(1.0, wide / max(luma(wide), 1e-3), LightTint), 0.5, 1.6);
	c = lerp(c, c * gain * tint, LightMatch);
	// The whole picture's colour, gentled, and kept off bright things so lights and fire keep their own
	const float3 whole = tex2Dlod(sGtrLight, float4(0.5, 0.5, 0, 6.0)).rgb;
	const float3 grade = clamp(lerp(1.0, whole / max(luma(whole), 1e-3), 0.6), 0.6, 1.6);
	c = lerp(c, c * grade, GradeMatch * (1.0 - saturate((max(c.r, max(c.g, c.b)) - 0.45) * 2.5)));
	const float haze = saturate((1.0 - exp(-max(z - HazeStart, 0.0) / HazeDistance)) * HazeStrength);
	return lerp(c, tex2Dlod(sGtrLight, float4(uv, 0, LightBlur + 1.0)).rgb, haze);
}

void PS_Composite(float4 pos : SV_Position, float2 uv : TEXCOORD, out float4 outColor : SV_Target0, out float4 outInfo : SV_Target1)
{
	const float3 host = tex2D(ReShade::BackBuffer, uv).rgb;
	outColor = float4(host, 1.0);
	outInfo = 0.0;
	if (!GtrActive)
		return;

	const float zh = host_linear(tex2Dlod(ReShade::DepthBuffer, float4(uv, 0, 0)).x);
	const int layer = guest_layer();
	const float4 guest = guest_color(layer, uv);
	const float zg = guest_depth(layer, uv);
	const float zgDrawn = guest.a > 0.5 ? zg : GTR_NO_GUEST;
	outInfo = float4(0.0, zgDrawn, zh, 0.0);

	if (DebugView == 1)
		outColor.rgb = bands(zh);
	else if (DebugView == 2)
		outColor.rgb = guest.a > 0.5 ? bands(zg) : host * 0.3;
	else if (DebugView == 3)
		outColor.rgb = guest.a > 0.5 ? float3(saturate((zh - zg) * 0.5 + 0.5), saturate((zg - zh) * 0.5 + 0.5), 0.0) : host * 0.3;
	else if (guest.a > 0.004 && zg < zh + DepthBias)
	{
		// What the guest covers only partly (see-through parts) is laid over the host by how much it covers
		outColor.rgb = lerp(host, DebugView == 4 ? guest.rgb : relight(guest.rgb, uv, zg), guest.a);
		outInfo = float4(guest.a > 0.5 ? 1.0 : 0.0, zgDrawn, zh, 0.0);
	}
}

/// The guest's shadow on the host's world. From each of the host's surfaces a ray is walked towards the host's sun through
/// the picture, and the surface is in shadow where the ray passes behind something the guest drew. Only what is in the
/// picture can cast one, and only by the side the camera sees, which is enough for a character and its bricks.
float shadowed(float3 at, float3 towards, float reach, float thickness, float2 tangent, float noise)
{
	[loop] for (int i = 0; i < GTR_SHADOW_STEPS; ++i)
	{
		const float t = 0.05 + reach * pow((i + noise) / GTR_SHADOW_STEPS, 1.7);
		const float3 p = at + towards * t;
		if (p.z < 0.2)
			break;
		const float2 where = float2(p.x / (p.z * tangent.x), -p.y / (p.z * tangent.y)) * 0.5 + 0.5;
		if (any(where < 0.0) || any(where > 1.0))
			break;
		const float behind = p.z - tex2Dlod(sGtrInfo, float4(where, 0, 0)).y;
		if (behind > 0.02 + 0.01 * t && behind < thickness)
			return 1.0;
	}
	return 0.0;
}

/// Where the light in the host's picture is. Each pixel weighs in by how far it stands out as bright against the picture as
/// a whole, and what is kept is the weight and the weight times where the light would be for that pixel, in the camera's
/// space: the smallest mip of this is then the sums over the whole picture, and their quotient the light's place. A bright
/// thing that faces up is ground a lamp shines down on, and its light is well above it; anything else is taken for the
/// lamp itself, or the wall beside it.
float4 PS_Lamp(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	// Not the script's mark, and not the sky
	if (!GtrActive || (uv.x < 0.03 && uv.y < 0.03))
		return 0.0;
	const float z = host_linear(tex2Dlod(ReShade::DepthBuffer, float4(uv, 0, 0)).x);
	if (z > LampFarthest)
		return 0.0;
	const float whole = luma(tex2Dlod(sGtrLight, float4(0.5, 0.5, 0, 6.0)).rgb);
	const float least = clamp(whole * LampContrast, LampBrightness, 0.9);
	const float bright = luma(tex2Dlod(sGtrLight, float4(uv, 0, 1.0)).rgb);
	const float weight = pow(saturate((bright - least) / (1.0 - least)), 2.0);
	if (weight <= 0.0)
		return 0.0;

	const float2 tangent = float2(ViewTangent * BUFFER_WIDTH * BUFFER_RCP_HEIGHT, ViewTangent);
	const float2 step = 1.0 / 64.0;
	const float3 at = view_position(uv, z, tangent);
	const float3 across = view_position(uv + float2(step.x, 0), host_linear(tex2Dlod(ReShade::DepthBuffer, float4(uv + float2(step.x, 0), 0, 0)).x), tangent) - at;
	const float3 down = view_position(uv + float2(0, step.y), host_linear(tex2Dlod(ReShade::DepthBuffer, float4(uv + float2(0, step.y), 0, 0)).x), tangent) - at;
	const float facesUp = saturate(dot(normalize(cross(across, down)), GtrUpView) * 2.0 - 0.6);
	const float3 light = at + GtrUpView * lerp(LampLift, LampLiftGround, facesUp);
	return float4(light * weight, weight);
}

float4 PS_Shadow(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	const float2 px = BUFFER_PIXEL_SIZE;
	const float4 info = tex2D(sGtrInfo, uv);
	if (!GtrActive || info.x > 0.5 || info.z > 300.0)
		return float4(0.0, 0.0, 0.0, 1.0);
	const float2 tangent = float2(ViewTangent * BUFFER_WIDTH * BUFFER_RCP_HEIGHT, ViewTangent);
	const float3 at = view_position(uv, info.z, tangent);

	// Contact shadows: the host's surfaces are darkened by how close the guest's surfaces are to them, in the world and
	// not just in the picture, so the dark hugs a character's feet and the foot of a brick and is gone a step away. What
	// stands on the ground is above it on screen, so that side counts for more.
	float contact = 0.0;
	if (ContactShadow > 0.0 && info.z < 200.0)
	{
		const float radius = clamp(ContactRadius / max(info.z, 0.5) * BUFFER_HEIGHT / (2.0 * ViewTangent), 3.0, 96.0);
		const float turn = frac(52.9829189 * frac(dot(pos.xy, float2(0.06711056, 0.00583715)))) * 6.2831853;
		float occluded = 0.0;
		[unroll] for (int i = 0; i < GTR_CONTACT_TAPS; ++i)
		{
			const float angle = i * 2.39996 + turn; // a golden-angle spiral, turned from pixel to pixel
			const float2 offset = float2(cos(angle), sin(angle)) * radius * sqrt((i + 0.5) / GTR_CONTACT_TAPS);
			const float2 where = uv + offset * px;
			const float4 beside = tex2Dlod(sGtrInfo, float4(where, 0, 0));
			if (beside.x > 0.5)
			{
				const float near = saturate(1.0 - distance(view_position(where, beside.y, tangent), at) / ContactRadius);
				occluded += near * near * (offset.y < 0.0 ? 1.0 : 0.5);
			}
		}
		contact = saturate(occluded / GTR_CONTACT_TAPS * ContactGain);
	}

	const float3 across = view_position(uv + float2(px.x, 0), tex2D(sGtrInfo, uv + float2(px.x, 0)).z, tangent) - at;
	const float3 down = view_position(uv + float2(0, px.y), tex2D(sGtrInfo, uv + float2(0, px.y)).z, tangent) - at;
	const float3 normal = normalize(cross(across, down));
	const float noise = frac(52.9829189 * frac(dot(pos.xy, float2(0.06711056, 0.00583715))));
	const float thickness = lerp(ShadowThickness, ShadowThicknessDown, abs(GtrUpView.z));

	// The sun's, or the moon's
	float sun = 0.0;
	const float sunStrength = saturate(GtrSunShadow * SunShadows);
	if (sunStrength > 0.0)
	{
		// Only surfaces turned away from the sun are left out. One the sun only grazes, the ground at sunset, keeps the whole
		// shadow, as GTA's own shadows there do
		const float facing = saturate(dot(normal, GtrSunView) * 25.0);
		if (facing > 0.0)
		{
			// By the frame's shadow map, or, for a guest that sends none, by what the picture itself shows of the guest
			const float mapped = sun_from_map(guest_layer(), at);
			sun = facing * (mapped >= 0.0 ? mapped : shadowed(at, GtrSunView, ShadowReach, thickness, tangent, noise));
		}
	}

	// A lamp's. The host tells nothing of its lamps, so the one that matters is looked for in its picture: the brightest
	// thing in it, weighed towards (see PS_Lamp), is taken for a light where it is seen, a little above what it lights,
	// and the guest casts a shadow away from it. It counts for less the more the sun's shadow does, the farther the surface
	// is from it, and the less there is in the picture that stands out as a light at all.
	float lamp = 0.0;
	if (LampShadows > 0.0 && sunStrength < LampSunLimit)
	{
		const float4 found = tex2Dlod(sGtrLamp, float4(0.5, 0.5, 0, GTR_LAMP_TOP_MIP));
		if (found.w > 1e-5)
		{
			const float3 light = found.xyz / found.w;
			const float3 towards = light - at;
			const float distance = length(towards);
			const float facing = saturate(dot(normal, towards / max(distance, 1e-3)) * 6.0);
			const float certain = saturate(found.w * LampCertainty) * saturate(1.0 - distance / LampRange) * (1.0 - sunStrength / LampSunLimit);
			if (facing > 0.0 && certain > 0.0 && distance > 0.3)
				lamp = facing * certain * shadowed(at, towards / distance, min(distance - 0.2, ShadowReach), thickness, tangent, noise);
		}
	}
	return float4(sun, contact, lamp, 1.0);
}

// What a shadow takes from each color: the sun's own light, which the sky's bluer light is left without. Weighed so that the
// brightness lost is the sun table's darkness. It is the host's, by the hour (sun_tint in script.cpp): GTA's sun is orange
// in the evening and white by day, while the sun is no lower at sunset than in the morning
float3 sun_tint()
{
	return GtrSunTint;
}

float3 final_picture(float4 pos, float2 uv)
{
	const float2 px = BUFFER_PIXEL_SIZE;
	// The mark is covered with the picture beside it
	if (GtrActive && GtrMarked && !GtrShowMark && pos.x < 2.0 * GTR_MARK_PIXELS && pos.y < GTR_MARK_PIXELS)
		uv.x += 2.0 * GTR_MARK_PIXELS * px.x;
	float3 color = tex2D(sGtrComposite, uv).rgb;
	if (!GtrActive || DebugView != 0)
		return color;
	const float4 info = tex2D(sGtrInfo, uv);

	if (EdgeSoftness > 0.0)
	{
		const float c1 = tex2D(sGtrInfo, uv + float2(px.x, 0)).x, c2 = tex2D(sGtrInfo, uv - float2(px.x, 0)).x;
		const float c3 = tex2D(sGtrInfo, uv + float2(0, px.y)).x, c4 = tex2D(sGtrInfo, uv - float2(0, px.y)).x;
		const float edge = saturate(abs(c1 - info.x) + abs(c2 - info.x) + abs(c3 - info.x) + abs(c4 - info.x));
		if (edge > 0.0)
		{
			const float3 around = tex2D(sGtrComposite, uv + float2(px.x, 0)).rgb + tex2D(sGtrComposite, uv - float2(px.x, 0)).rgb
				+ tex2D(sGtrComposite, uv + float2(0, px.y)).rgb + tex2D(sGtrComposite, uv - float2(0, px.y)).rgb;
			color = lerp(color, (color * 2.0 + around) / 6.0, edge * EdgeSoftness);
		}
	}

	// The guest's shadows on the host's surfaces, their speckle smoothed: the one the sun casts and the one of contact.
	// Where both fall the darker counts, as one shadow inside another adds nothing.
	if (info.x < 0.5)
	{
		float3 shade = 0.0;
		[unroll] for (int y = -1; y <= 1; ++y)
			[unroll] for (int x = -1; x <= 1; ++x)
				shade += tex2D(sGtrShadow, uv + float2(x, y) * 1.5 * px).xyz;
		shade /= 9.0;
		const float3 sunShade = min(saturate(GtrSunShadow * SunShadows) * sun_tint(), 0.9) * shade.x;
		color *= 1.0 - max(max(sunShade, ContactShadow * shade.y), saturate(LampShadows) * shade.z);
	}

	return color;
}

float3 PS_Final(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float3 color = final_picture(pos, uv);
	if (GtrActive && DebugView == 5)
	{
		color = tex2D(sGtrShadow, uv).xyz + color * 0.25;
		// Where the lamp was found, as a white square
		const float4 found = tex2Dlod(sGtrLamp, float4(0.5, 0.5, 0, GTR_LAMP_TOP_MIP));
		if (found.w > 1e-5)
		{
			const float3 light = found.xyz / found.w;
			const float2 tangent = float2(ViewTangent * BUFFER_WIDTH * BUFFER_RCP_HEIGHT, ViewTangent);
			const float2 where = float2(light.x / (light.z * tangent.x), -light.y / (light.z * tangent.y)) * 0.5 + 0.5;
			if (light.z > 0.2 && all(abs(uv - where) < float2(6.0, 6.0) * BUFFER_PIXEL_SIZE))
				color = 1.0;
		}
	}
	if (GtrActive && GtrGui)
	{
		// Its colour is already multiplied by its alpha
		const float4 gui = tex2D(sGtrGui, uv);
		color = color * (1.0 - gui.a) + gui.rgb;
	}
	return color;
}

technique GtrPassthrough < ui_tooltip = "Vanadium passthrough: shows the guest while its frames are arriving."; >
{
	pass Light
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Light;
		RenderTarget = GtrLightTex;
	}
	pass Composite
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Composite;
		RenderTarget0 = GtrCompositeTex;
		RenderTarget1 = GtrInfoTex;
	}
	pass Lamp
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Lamp;
		RenderTarget = GtrLampTex;
	}
	pass Shadow
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Shadow;
		RenderTarget = GtrShadowTex;
	}
	pass Final
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Final;
	}
}
