// A stand-in for GTA V that tests the host half of the passthrough without the game: a D3D11 window with a reversed-Z depth
// buffer, like GTA's, showing the scene host/fakehost.py ray-traces (a checkerboard ground and a pillar only the host has). It
// drives the guest over the link the way the real script does, while ReShade (dxgi.dll beside the exe) runs GtrPassthrough.fx
// with the real GtrCompositor.addon64. Just before it ends it asks the add-on to save the finished picture.
//
//   fakegta.exe eyeX eyeY eyeZ targetX targetY targetZ fovY seconds capture.bmp [still|orbit|orbit-unsynced]
//
// still (the default) holds the camera where it is given, for a picture that can be checked pixel by pixel.
//
// orbit swings the camera round the target fast, which is what shows a guest frame put with the wrong host picture: the
// guest's frames arrive some frames after the camera they were asked for. As the real script does, it draws each picture from
// the camera of the newest guest frame there is, marks the picture with the tick's number and files that frame under it, so
// the compositor can put the two together. Inside each brick the host draws a magenta box, a little smaller: with the guest's
// brick exactly over it none of it shows, and any that does is the two pictures being of different cameras.
//
// orbit-unsynced swings the same way but draws from the newest camera and marks nothing, which is how things were before:
// magenta shows, and that it does is the check that the test can fail. It also leaves the compositor free running: held for
// each new guest frame, the newest frame is new enough that next to no magenta shows.
//
// host/test_compositor.py runs all three and checks the pictures.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gtr_frame.h"
#include "../src/gta_math.h"
#include "../src/link.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace
{
	constexpr int kWidth = 1280, kHeight = 720;
	constexpr float kNear = 0.15f, kFar = 10000.0f;
	// The same scene as host/fakehost.py, in host space (metres, Z up)
	constexpr double kOrigin[3] = {-75.0, -818.0, 326.0};
	constexpr float kSky[4] = {0.60f, 0.70f, 0.85f, 1.0f};
	constexpr float kPillarColor[3] = {0.85f, 0.75f, 0.10f};
	// The direction towards the sun in the mode that has one (a unit vector), and how dark its shadows are; test_compositor.py
	// has the same
	constexpr float kSun[3] = {-0.48f, 0.36f, 0.80f};
	constexpr float kSunShadow = 0.5f;
	constexpr float kMagenta[3] = {1.0f, 0.0f, 1.0f};
	// How much smaller than its brick, each way, the box inside it is
	constexpr float kInset = 0.06f;
	// How long the picture gets to settle after the capture is asked for
	constexpr double kCaptureSeconds = 1.0;
	constexpr int kHiddenBoxes = 16;
	constexpr UINT kBoxVertices = 36;
	constexpr double kOrbitDegreesPerSecond = 100.0;
	constexpr size_t kHistory = 256;

	// The bricks the guest is asked for: centre relative to the origin, size along the brick's own axes, yaw, colour
	struct Brick
	{
		const char *id;
		float centre[3];
		float size[3];
		float yaw;
		float color[3];
	};
	constexpr Brick kBricks[] = {
		{"red", {0.0f, 6.0f, 0.5f}, {1.0f, 1.0f, 1.0f}, 0.0f, {0.77f, 0.16f, 0.11f}},
		{"green", {3.0f, 8.0f, 0.75f}, {2.0f, 0.5f, 1.5f}, 30.0f, {0.29f, 0.59f, 0.29f}},
		{"blue", {-2.5f, 5.0f, 1.5f}, {0.5f, 0.5f, 3.0f}, 0.0f, {0.05f, 0.41f, 0.67f}},
		{"floating", {0.5f, 9.0f, 3.0f}, {1.5f, 1.5f, 0.4f}, 65.0f, {0.96f, 0.80f, 0.19f}},
	};

	struct Vertex
	{
		float x, y, z;
		float r, g, b;
		float checker;
	};

	// Positions are relative to the origin, so floats keep their precision this far from the world's centre
	const char *kShader = R"(
cbuffer Frame : register(b0) { float4x4 viewProj; };
struct VSOut { float4 pos : SV_Position; float3 world : WORLD; float3 color : COLOR; float checker : CHECKER; };
VSOut vs(float3 p : POSITION, float3 c : COLOR, float k : CHECKER)
{
	VSOut o; o.pos = mul(viewProj, float4(p, 1)); o.world = p; o.color = c; o.checker = k; return o;
}
float4 ps(VSOut i) : SV_Target
{
	if (i.checker > 0.5)
	{
		bool odd = fmod(abs(floor(i.world.x) + floor(i.world.y)), 2.0) > 0.5;
		return float4(odd ? float3(0.42, 0.42, 0.44) : float3(0.30, 0.30, 0.32), 1);
	}
	return float4(i.color, 1);
}
)";

	struct Mat
	{
		float m[4][4];
	};
	constexpr Mat kIdentity = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};

	Mat mul(const Mat &a, const Mat &b)
	{
		Mat r = {};
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				for (int k = 0; k < 4; ++k)
					r.m[i][j] += a.m[i][k] * b.m[k][j];
		return r;
	}

	struct Camera
	{
		double eye[3];
		float right[3], forward[3], up[3];
		float fov;
	};

	Camera look_at(const double eye[3], const double target[3], float fov)
	{
		Camera c = {};
		std::memcpy(c.eye, eye, sizeof(c.eye));
		c.fov = fov;
		double f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
		const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
		for (double &v : f)
			v /= fl;
		// right = forward x up(0, 0, 1), up = right x forward
		double r[3] = {f[1], -f[0], 0.0};
		const double rl = std::sqrt(r[0] * r[0] + r[1] * r[1]);
		r[0] /= rl;
		r[1] /= rl;
		const double u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
		for (int i = 0; i < 3; ++i)
		{
			c.forward[i] = float(f[i]);
			c.right[i] = float(r[i]);
			c.up[i] = float(u[i]);
		}
		return c;
	}

	Mat view_proj(const Camera &c, float aspect)
	{
		const float e[3] = {float(c.eye[0] - kOrigin[0]), float(c.eye[1] - kOrigin[1]), float(c.eye[2] - kOrigin[2])};
		const float *r = c.right, *u = c.up, *f = c.forward;
		// view: rows are right, up, -forward (right-handed, the camera looks down -z)
		const Mat v = {{{r[0], r[1], r[2], -(r[0] * e[0] + r[1] * e[1] + r[2] * e[2])},
						{u[0], u[1], u[2], -(u[0] * e[0] + u[1] * e[1] + u[2] * e[2])},
						{-f[0], -f[1], -f[2], (f[0] * e[0] + f[1] * e[1] + f[2] * e[2])},
						{0, 0, 0, 1}}};
		const float t = std::tan(c.fov * 3.14159265f / 360.0f);
		// reversed Z with a finite far plane: depth = n (f - dist) / (dist (f - n)), 1 at the near plane
		const Mat p = {{{1 / (aspect * t), 0, 0, 0},
						{0, 1 / t, 0, 0},
						{0, 0, kNear / (kFar - kNear), kNear * kFar / (kFar - kNear)},
						{0, 0, -1, 0}}};
		return mul(p, v);
	}

	/// Whether the script's way of turning GTA's camera rotation into axes agrees with the axes built here from a look-at.
	bool rotation_math_agrees(const Camera &c)
	{
		const float pitch = std::asin(c.forward[2]) / gta::kDegrees, yaw = std::atan2(-c.forward[0], c.forward[1]) / gta::kDegrees;
		const gta::Basis basis = gta::basis_from_rotation(pitch, 0.0f, yaw);
		float worst = 0.0f;
		for (int i = 0; i < 3; ++i)
			worst = std::max({worst, std::fabs(basis.right[i] - c.right[i]), std::fabs(basis.forward[i] - c.forward[i]), std::fabs(basis.up[i] - c.up[i])});
		return worst < 1e-4f;
	}

	/// A box with its centre and size along its own axes, turned by yaw about +Z, in one flat colour.
	void add_box(std::vector<Vertex> &out, const float centre[3], const float size[3], float yawDegrees, const float color[3])
	{
		const float a = yawDegrees * 3.14159265f / 180.0f, cs = std::cos(a), sn = std::sin(a);
		float corners[8][3];
		for (int i = 0; i < 8; ++i)
		{
			const float lx = ((i & 1) ? 0.5f : -0.5f) * size[0], ly = ((i & 2) ? 0.5f : -0.5f) * size[1], lz = ((i & 4) ? 0.5f : -0.5f) * size[2];
			corners[i][0] = centre[0] + lx * cs - ly * sn;
			corners[i][1] = centre[1] + lx * sn + ly * cs;
			corners[i][2] = centre[2] + lz;
		}
		const int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {1, 5, 7, 3}, {3, 7, 6, 2}, {2, 6, 4, 0}};
		for (const auto &q : faces)
			for (int k : {0, 1, 2, 0, 2, 3})
				out.push_back({corners[q[k]][0], corners[q[k]][1], corners[q[k]][2], color[0], color[1], color[2], 0});
	}

	/// A square of the picture's pixels, as two triangles in front of everything; drawn with no camera at all.
	void add_square(std::vector<Vertex> &out, int left, int top, int size, float r, float g, float b)
	{
		const float x0 = float(left) / kWidth * 2 - 1, x1 = float(left + size) / kWidth * 2 - 1;
		const float y0 = 1 - float(top) / kHeight * 2, y1 = 1 - float(top + size) / kHeight * 2;
		const float corners[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
		for (int k : {0, 1, 2, 0, 2, 3})
			out.push_back({corners[k][0], corners[k][1], 1.0f, r, g, b, 0});
	}

	std::string brick_message(const Brick &b)
	{
		char line[512];
		snprintf(line, sizeof(line), "{\"t\":\"brick\",\"id\":\"%s\",\"pos\":[%.4f,%.4f,%.4f],\"size\":[%.3f,%.3f,%.3f],\"yaw\":%.2f,\"color\":[%.3f,%.3f,%.3f]}",
			b.id, kOrigin[0] + b.centre[0], kOrigin[1] + b.centre[1], kOrigin[2] + b.centre[2], b.size[0], b.size[1], b.size[2], b.yaw, b.color[0], b.color[1], b.color[2]);
		return line;
	}

	/// The CameraId of the newest frame the guest has published, or 0: the id of the cam message it was drawn from.
	int64_t newest_guest_frame()
	{
		static const GtrFrameHeader *header = nullptr;
		if (header == nullptr)
		{
			const HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, GTR_FRAME_MAPPING_NAME);
			header = mapping != nullptr ? static_cast<const GtrFrameHeader *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, GTR_FRAME_HEADER_BYTES)) : nullptr;
			if (header == nullptr)
				return 0;
		}
		const int32_t index = header->LatestSlot;
		if (header->Magic != GTR_FRAME_MAGIC || index < 0 || index >= GTR_FRAME_SLOTS || (header->Slots[index].Sequence & 1) != 0)
			return 0;
		return header->Slots[index].CameraId;
	}

	LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		if (m == WM_DESTROY)
		{
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(h, m, w, l);
	}
}

int main(int argc, char **argv)
{
	if (argc < 10)
	{
		std::puts("usage: fakegta eyeX eyeY eyeZ targetX targetY targetZ fovY seconds capture.bmp [still|sun|paint|orbit|orbit-unsynced]");
		return 2;
	}
	const double eye[3] = {std::atof(argv[1]), std::atof(argv[2]), std::atof(argv[3])};
	const double target[3] = {std::atof(argv[4]), std::atof(argv[5]), std::atof(argv[6])};
	const float fov = float(std::atof(argv[7]));
	const Camera start = look_at(eye, target, fov);
	const double seconds = std::atof(argv[8]);
	const char *capturePath = argv[9];
	const std::string mode = argc > 10 ? argv[10] : "still";
	const bool sunny = mode == "sun", painting = mode == "paint";
	const bool orbits = mode == "orbit" || mode == "orbit-unsynced", synced = mode == "orbit";
	if (!rotation_math_agrees(start))
	{
		std::puts("gta_math.h's camera axes disagree with the look-at's");
		return 4;
	}

	// What the real script publishes for the compositor: the camera's clip planes, the request for a picture, and which
	// guest frame goes with which tick
	const HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(GtrHostState), GTR_HOST_MAPPING_NAME);
	auto *state = mapping != nullptr ? static_cast<GtrHostState *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GtrHostState))) : nullptr;
	if (state == nullptr)
	{
		std::puts("could not create the host state mapping");
		return 1;
	}
	std::memset(state, 0, sizeof(*state));
	state->Version = 1;
	state->Active = 1;
	state->NearClip = kNear;
	state->FarClip = kFar;
	state->FreeRunning = orbits && !synced ? 1 : 0;
	strncpy_s(state->CapturePath, capturePath, _TRUNCATE);
	state->Magic = GTR_HOST_MAGIC;
	// Painting, as the real script paints a person or a prop the paintball gun hit: the pillar, red, in a box of its own
	GtrPaintState *paint = nullptr;
	if (painting)
	{
		const HANDLE paintMapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(GtrPaintState), GTR_PAINT_MAPPING_NAME);
		paint = paintMapping != nullptr ? static_cast<GtrPaintState *>(MapViewOfFile(paintMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GtrPaintState))) : nullptr;
		if (paint != nullptr)
		{
			std::memset(paint, 0, sizeof(*paint));
			paint->Magic = GTR_PAINT_MAGIC;
		}
	}

	WNDCLASSW wc = {};
	wc.lpfnWndProc = wndproc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"FakeGTA";
	RegisterClassW(&wc);
	RECT rc = {0, 0, kWidth, kHeight};
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowW(L"FakeGTA", L"Fake GTA (passthrough test)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40,
		rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);

	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Width = kWidth;
	sd.BufferDesc.Height = kHeight;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	IDXGISwapChain *swap = nullptr;
	if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx)))
	{
		std::puts("D3D11 init failed");
		return 1;
	}

	ID3D11Texture2D *back = nullptr;
	swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back));
	ID3D11RenderTargetView *rtv = nullptr;
	dev->CreateRenderTargetView(back, nullptr, &rtv);
	D3D11_TEXTURE2D_DESC dd = {};
	dd.Width = kWidth;
	dd.Height = kHeight;
	dd.MipLevels = 1;
	dd.ArraySize = 1;
	dd.Format = DXGI_FORMAT_D32_FLOAT;
	dd.SampleDesc.Count = 1;
	dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	ID3D11Texture2D *depth = nullptr;
	dev->CreateTexture2D(&dd, nullptr, &depth);
	ID3D11DepthStencilView *dsv = nullptr;
	if (depth == nullptr || FAILED(dev->CreateDepthStencilView(depth, nullptr, &dsv)))
	{
		std::puts("could not create the depth buffer");
		return 1;
	}
	D3D11_DEPTH_STENCIL_DESC dsd = {};
	dsd.DepthEnable = TRUE;
	dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dsd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL; // reversed Z
	ID3D11DepthStencilState *dss = nullptr;
	dev->CreateDepthStencilState(&dsd, &dss);
	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	ID3D11RasterizerState *rs = nullptr;
	dev->CreateRasterizerState(&rd, &rs);

	ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
	if (FAILED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
		FAILED(D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err)))
	{
		std::printf("shader: %s\n", err ? static_cast<const char *>(err->GetBufferPointer()) : "?");
		return 1;
	}
	ID3D11VertexShader *vs = nullptr;
	ID3D11PixelShader *ps = nullptr;
	dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
	dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
	const D3D11_INPUT_ELEMENT_DESC layout[] = {
		{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"CHECKER", 0, DXGI_FORMAT_R32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
	};
	ID3D11InputLayout *il = nullptr;
	dev->CreateInputLayout(layout, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il);

	// The host's own scene, relative to the origin: the ground at its height and the pillar
	std::vector<Vertex> verts;
	const float g = 400.0f;
	const float quad[6][2] = {{-g, -g}, {g, -g}, {g, g}, {-g, -g}, {g, g}, {-g, g}};
	for (const auto &q : quad)
		verts.push_back({q[0], q[1], 0, 0, 0, 0, 1});
	const float pillarCentre[3] = {1.2f, 4.5f, 1.25f}, pillarSize[3] = {0.6f, 0.6f, 2.5f};
	add_box(verts, pillarCentre, pillarSize, 15.0f, kPillarColor);
	if (orbits)
	{
		for (const Brick &brick : kBricks)
		{
			const float size[3] = {brick.size[0] - 2 * kInset, brick.size[1] - 2 * kInset, brick.size[2] - 2 * kInset};
			add_box(verts, brick.centre, size, brick.yaw, kMagenta);
		}
	}
	// ReShade's depth detection takes no notice of a frame with a single depth buffer and eight draw calls or fewer (so
	// emulators that present more often than they render don't confuse it). A game makes thousands; here some boxes under the
	// ground, where they can't be seen, are each drawn with a call of their own.
	const UINT visible = UINT(verts.size());
	for (int i = 0; i < kHiddenBoxes; ++i)
	{
		const float centre[3] = {float(i) - 8.0f, 1.0f, -3.0f}, size[3] = {0.5f, 0.5f, 0.5f};
		add_box(verts, centre, size, 0.0f, kPillarColor);
	}
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = UINT(verts.size() * sizeof(Vertex));
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	D3D11_SUBRESOURCE_DATA init = {verts.data()};
	ID3D11Buffer *vb = nullptr;
	dev->CreateBuffer(&bd, &init, &vb);
	// The mark's two squares, rewritten every frame
	D3D11_BUFFER_DESC md = {};
	md.ByteWidth = 12 * sizeof(Vertex);
	md.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	md.Usage = D3D11_USAGE_DYNAMIC;
	md.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ID3D11Buffer *markBuffer = nullptr;
	dev->CreateBuffer(&md, nullptr, &markBuffer);
	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = sizeof(Mat);
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ID3D11Buffer *cb = nullptr;
	dev->CreateBuffer(&cbd, nullptr, &cb);
	const auto set_matrix = [&](const Mat &matrix) {
		// HLSL reads a float4x4 in a cbuffer column-major, so the row-major matrix goes in transposed
		Mat transposed;
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				transposed.m[i][j] = matrix.m[j][i];
		D3D11_MAPPED_SUBRESOURCE mapped;
		ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		std::memcpy(mapped.pData, &transposed, sizeof(transposed));
		ctx->Unmap(cb, 0);
	};

	// The same link the real script uses
	GuestLink link;
	int introducedTo = 0;
	bool captureAsked = false;
	// The camera each frame asked the guest for, by the frame's number
	std::vector<Camera> asked(kHistory, start);
	const double radius = std::hypot(eye[0] - target[0], eye[1] - target[1]);
	const double startAngle = std::atan2(eye[1] - target[1], eye[0] - target[0]);
	const auto began = std::chrono::steady_clock::now();
	int frame = 0;
	MSG msg = {};
	while (msg.message != WM_QUIT)
	{
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
		if (t > seconds + kCaptureSeconds)
			break;
		if (t > seconds && !captureAsked)
		{
			state->CaptureRequest++;
			captureAsked = true;
		}
		++frame;
		state->Heartbeat++;

		// Where the camera is now, which the guest is asked to draw from
		Camera now = start;
		if (orbits)
		{
			const double angle = startAngle + t * kOrbitDegreesPerSecond * 3.14159265358979 / 180.0;
			const double at[3] = {target[0] + radius * std::cos(angle), target[1] + radius * std::sin(angle), eye[2]};
			now = look_at(at, target, fov);
		}
		asked[frame % kHistory] = now;

		link.poll();
		if (link.connected())
		{
			if (link.generation() != introducedTo)
			{
				introducedTo = link.generation();
				char line[256];
				snprintf(line, sizeof(line), "{\"t\":\"origin\",\"pos\":[%.4f,%.4f,%.4f]}", kOrigin[0], kOrigin[1], kOrigin[2]);
				link.send_line(line);
				link.send_line("{\"t\":\"clear\"}");
				for (const Brick &brick : kBricks)
					link.send_line(brick_message(brick));
				if (sunny)
				{
					// As the real script does: the guest then sends its shadow map, which is what its shadows are cast from
					snprintf(line, sizeof(line), "{\"t\":\"light\",\"sun\":[%.4f,%.4f,%.4f]}", kSun[0], kSun[1], kSun[2]);
					link.send_line(line);
				}
				else
				{
					link.send_line("{\"t\":\"light\",\"clock\":14}");
				}
			}
			char line[512];
			snprintf(line, sizeof(line), "{\"t\":\"cam\",\"id\":%d,\"pos\":[%.5f,%.5f,%.5f],\"right\":[%.7f,%.7f,%.7f],\"fwd\":[%.7f,%.7f,%.7f],"
				"\"up\":[%.7f,%.7f,%.7f],\"fov\":%.4f,\"w\":%d,\"h\":%d}",
				frame, now.eye[0], now.eye[1], now.eye[2], now.right[0], now.right[1], now.right[2],
				now.forward[0], now.forward[1], now.forward[2], now.up[0], now.up[1], now.up[2], now.fov, kWidth, kHeight);
			link.send_line(line);
		}

		// The camera this picture is drawn from: the newest, or, to stay with the guest, the one the newest guest frame was
		// drawn from, which is a few frames old
		Camera drawn = now;
		const int tick = frame % GTR_HOST_TICKS;
		const int64_t newest = synced ? newest_guest_frame() : 0;
		const bool matched = newest > 0 && newest <= frame && frame - newest < int64_t(kHistory);
		if (matched)
			drawn = asked[newest % kHistory];
		state->TickCamera[tick] = matched ? newest : 0;
		if (paint != nullptr)
		{
			// The pillar's box (see add_box below: turned 15 degrees), off the ground by a little, in the drawn camera's space
			const float turn = 15.0f * 3.14159265f / 180.0f;
			const float centre[3] = {float(kOrigin[0] + 1.2 - drawn.eye[0]), float(kOrigin[1] + 4.5 - drawn.eye[1]), float(kOrigin[2] + 1.32 - drawn.eye[2])};
			const float boxAxes[3][3] = {{std::cos(turn), std::sin(turn), 0.0f}, {-std::sin(turn), std::cos(turn), 0.0f}, {0.0f, 0.0f, 1.0f}};
			const float half[3] = {0.312f, 0.312f, 1.3f};
			const float *cameraAxes[3] = {drawn.right, drawn.up, drawn.forward};
			GtrPaintBox &box = paint->Boxes[0];
			for (int i = 0; i < 3; ++i)
			{
				box.Centre[i] = cameraAxes[i][0] * centre[0] + cameraAxes[i][1] * centre[1] + cameraAxes[i][2] * centre[2];
				for (int j = 0; j < 3; ++j)
					box.Across[i][j] = (cameraAxes[j][0] * boxAxes[i][0] + cameraAxes[j][1] * boxAxes[i][1] + cameraAxes[j][2] * boxAxes[i][2]) / half[i];
			}
			box.Color[0] = 1.0f;
			box.Color[1] = 0.0f;
			box.Color[2] = 0.0f;
			box.Strength = 0.85f;
			paint->Count = 1;
			state->TanHalfFov = std::tan(drawn.fov * 3.14159265358979f / 360.0f);
		}
		if (sunny)
		{
			// As the real script does: the sun in the camera's space, for the shadows the guest casts on the host
			for (int axis = 0; axis < 3; ++axis)
			{
				const float *along = axis == 0 ? drawn.right : axis == 1 ? drawn.up : drawn.forward;
				state->SunView[axis] = along[0] * kSun[0] + along[1] * kSun[1] + along[2] * kSun[2];
			}
			state->SunShadow = kSunShadow;
			state->TanHalfFov = std::tan(drawn.fov * 3.14159265358979f / 360.0f);
		}

		ctx->ClearRenderTargetView(rtv, kSky);
		ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
		ctx->OMSetRenderTargets(1, &rtv, dsv);
		ctx->OMSetDepthStencilState(dss, 0);
		ctx->RSSetState(rs);
		const D3D11_VIEWPORT viewport = {0, 0, float(kWidth), float(kHeight), 0, 1};
		ctx->RSSetViewports(1, &viewport);
		const UINT stride = sizeof(Vertex), offset = 0;
		ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
		ctx->IASetInputLayout(il);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(vs, nullptr, 0);
		ctx->VSSetConstantBuffers(0, 1, &cb);
		ctx->PSSetShader(ps, nullptr, 0);
		set_matrix(view_proj(drawn, float(kWidth) / kHeight));
		ctx->Draw(visible, 0);
		for (UINT box = 0; box < kHiddenBoxes; ++box)
			ctx->Draw(kBoxVertices, visible + box * kBoxVertices);

		if (matched)
		{
			// The mark, as the script's rectangles draw it: the tick's number, a channel a bit, and then its opposite
			std::vector<Vertex> mark;
			const float r = (tick & 1) ? 1.0f : 0.0f, gr = (tick & 2) ? 1.0f : 0.0f, b = (tick & 4) ? 1.0f : 0.0f;
			add_square(mark, 0, 0, GTR_MARK_PIXELS, r, gr, b);
			add_square(mark, GTR_MARK_PIXELS, 0, GTR_MARK_PIXELS, 1 - r, 1 - gr, 1 - b);
			D3D11_MAPPED_SUBRESOURCE mapped;
			ctx->Map(markBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
			std::memcpy(mapped.pData, mark.data(), mark.size() * sizeof(Vertex));
			ctx->Unmap(markBuffer, 0);
			ctx->IASetVertexBuffers(0, 1, &markBuffer, &stride, &offset);
			set_matrix(kIdentity);
			ctx->Draw(UINT(mark.size()), 0);
		}
		swap->Present(1, 0);
	}

	std::printf("frames: %d, capture %s\n", frame, state->CaptureDone == state->CaptureRequest && captureAsked ? "saved" : "NOT saved");
	return state->CaptureDone == state->CaptureRequest && captureAsked ? 0 : 3;
}
