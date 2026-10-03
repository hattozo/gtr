// GtrCompositor.addon64: the ReShade add-on that puts the guest's frames in front of GtrPassthrough.fx.
//
// Each frame it copies the guest's colour and depth (shared/gtr_frame.h) into textures the effect samples, and tells the
// effect whether to show them. It works by itself, showing the newest frame; a host script that knows the camera's clip
// planes, or wants the guest hidden or a picture saved, says so through GtrHostState. A script that puts the host's camera
// where each guest frame was drawn from also says which frame goes with which of its ticks, and the frames for the last few
// ticks are then all kept ready: the effect reads off the picture which tick it is of, and samples that one.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <reshade.hpp>

#include "gtr_frame.h"

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "GTR passthrough compositor";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Composites Vanadium's frames into the game by depth (GtrPassthrough.fx).";

namespace
{
	constexpr const char *kEffect = "GtrPassthrough.fx";
	constexpr DWORD kRetryMilliseconds = 1000;
	// With no new frame for this long the guest has stopped or gone, and its last picture must not stay on screen
	constexpr DWORD kStaleMilliseconds = 1000;
	constexpr DWORD kPausedMilliseconds = 300;
	// A frame is some megabytes to copy, so no more than this many are taken up in one of the host's frames; the rest follow
	constexpr int kUploadsPerPresent = 3;
	// The longest one of the host's frames is held for the guest's next (GtrHostState::FreeRunning). Long enough for a guest
	// frame to get through the GPU while the host isn't feeding it (the guest drew 88 a second with the GPU to itself), and
	// short enough that a guest that can't keep up costs the host no more than this a frame
	constexpr double kGuestWaitMilliseconds = 25.0;
	// A guest that has published nothing for this long has stalled or stopped, and isn't waited for at all
	constexpr double kGuestStalledMilliseconds = 200.0;

	struct Mapping
	{
		HANDLE handle = nullptr;
		const uint8_t *view = nullptr;
		DWORD nextAttempt = 0;
	};
	Mapping g_frames, g_host, g_paint;
	int64_t g_slotStride = 0;
	DWORD g_shownAt = 0;
	int32_t g_captured = 0;

	struct Texture
	{
		resource tex = {0};
		resource_view srv = {0};
	};
	// One guest frame ready to sample: the newest in the first of these, or, with a script that marks its ticks, the frames
	// chosen at the last ticks. Several ticks choose the same frame when the host draws faster than the guest, so a frame is
	// kept once, whichever ticks want it, and the effect is told which layer each tick's is
	struct Layer
	{
		Texture color, depth;
		// The frame's shadow map, and what turns a place in the frame's camera's space into a place in the map's: three rows
		// for across, up and into the map, and the tangent of half the map's field of view (0 when the frame has no map)
		Texture light;
		float lightRows[3][4] = {};
		float lightTan = 0.0f;
		// Which frame it holds: the ring slot it came from and that slot's sequence, which only ever grows
		int64_t frame = 0;
		// The same by the name the script knows it by; 0 while the layer holds nothing whole
		int64_t cameraId = 0;
		// The part of the picture the frame has anything in, as fractions of it: left, top, right, bottom. Only that part is
		// copied, and the effect takes the rest for empty
		float rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
	};
	Layer g_layers[GTR_HOST_TICKS];
	// The guest's interface, of the newest frame: it goes over the whole picture, whichever frame is under it
	Texture g_gui;
	uint32_t g_guiWidth = 0, g_guiHeight = 0, g_guiBlueFirst = 0;
	int64_t g_guiFrame = 0;
	// The guest's number for the picture of its interface the texture holds (GtrFrameSlot::GuiVersion), 0 for none known
	uint64_t g_guiVersion = 0;
	bool g_hasGui = false;
	// The effect's file, watched so that a changed effect is taken up by a game that is running
	char g_effectPath[MAX_PATH] = {};
	FILETIME g_effectWritten = {};
	DWORD g_effectCheckedAt = 0;
	uint32_t g_width = 0, g_height = 0;
	bool g_hasFrame = false;
	// The guest's published event, the frame count when one of the host's frames last went ahead, and when that count last
	// grew. The event is kept open for good: a guest that restarts opens the same one again, since this handle keeps it alive
	HANDLE g_published = nullptr;
	int64_t g_seenPublished = 0;
	double g_publishedAt = 0.0;

	void close(Mapping &mapping)
	{
		if (mapping.view != nullptr)
			UnmapViewOfFile(mapping.view);
		if (mapping.handle != nullptr)
			CloseHandle(mapping.handle);
		mapping = Mapping();
	}

	/// Maps a named mapping once its writer has created it and written the magic; retried every second until then.
	bool open(Mapping &mapping, const char *name, uint32_t magic, DWORD access, size_t bytes)
	{
		if (mapping.view != nullptr)
			return true;
		if (GetTickCount() < mapping.nextAttempt)
			return false;
		mapping.nextAttempt = GetTickCount() + kRetryMilliseconds;
		mapping.handle = OpenFileMappingA(access, FALSE, name);
		if (mapping.handle == nullptr)
			return false;
		mapping.view = static_cast<const uint8_t *>(MapViewOfFile(mapping.handle, access, 0, 0, bytes));
		uint32_t found = 0;
		if (mapping.view != nullptr)
			std::memcpy(&found, mapping.view, sizeof(found));
		if (found != magic)
		{
			const DWORD retry = mapping.nextAttempt;
			close(mapping);
			mapping.nextAttempt = retry;
			return false;
		}
		return true;
	}

	bool open_frames()
	{
		if (g_frames.view != nullptr)
			return true;
		if (GetTickCount() < g_frames.nextAttempt)
			return false;
		// The header says how large the whole mapping is, so it is mapped alone first
		Mapping header;
		if (!open(header, GTR_FRAME_MAPPING_NAME, GTR_FRAME_MAGIC, FILE_MAP_READ, GTR_FRAME_HEADER_BYTES))
		{
			g_frames.nextAttempt = GetTickCount() + kRetryMilliseconds;
			return false;
		}
		const auto *described = reinterpret_cast<const GtrFrameHeader *>(header.view);
		const bool known = described->Version == GTR_FRAME_VERSION && described->SlotCount == GTR_FRAME_SLOTS;
		g_slotStride = described->SlotStride;
		close(header);
		if (!known)
			return false;
		g_frames.nextAttempt = 0;
		if (!open(g_frames, GTR_FRAME_MAPPING_NAME, GTR_FRAME_MAGIC, FILE_MAP_READ, size_t(GTR_FRAME_HEADER_BYTES + g_slotStride * GTR_FRAME_SLOTS)))
			return false;
		for (Layer &layer : g_layers)
			layer.frame = 0;
		reshade::log::message(reshade::log::level::info, "GTR: connected to the guest's frames");
		return true;
	}

	void destroy_layers(device *dev)
	{
		for (Layer &layer : g_layers)
		{
			for (Texture *texture : {&layer.color, &layer.depth, &layer.light})
			{
				if (texture->srv.handle != 0)
					dev->destroy_resource_view(texture->srv);
				if (texture->tex.handle != 0)
					dev->destroy_resource(texture->tex);
			}
			layer = Layer();
		}
		g_width = g_height = 0;
		g_hasFrame = false;
	}

	bool create_texture(device *dev, Texture &texture, uint32_t width, uint32_t height, format fmt)
	{
		if (!dev->create_resource(
				resource_desc(width, height, 1, 1, fmt, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest),
				nullptr, resource_usage::shader_resource, &texture.tex))
			return false;
		return dev->create_resource_view(texture.tex, resource_usage::shader_resource, resource_view_desc(fmt), &texture.srv);
	}

	/// The effect's textures are GTRCOLOR0, GTRDEPTH0, GTRCOLOR1 and so on.
	void bind(effect_runtime *runtime)
	{
		for (int i = 0; i < GTR_HOST_TICKS; ++i)
		{
			char name[16];
			snprintf(name, sizeof(name), "GTRCOLOR%d", i);
			runtime->update_texture_bindings(name, g_layers[i].color.srv, g_layers[i].color.srv);
			snprintf(name, sizeof(name), "GTRDEPTH%d", i);
			runtime->update_texture_bindings(name, g_layers[i].depth.srv, g_layers[i].depth.srv);
			snprintf(name, sizeof(name), "GTRSUNMAP%d", i);
			runtime->update_texture_bindings(name, g_layers[i].light.srv, g_layers[i].light.srv);
		}
	}

	/// Copies a ring slot's frame into a layer, unless the layer already holds it. True when it copied.
	bool upload(effect_runtime *runtime, command_list *cmd_list, Layer &layer, int32_t slotIndex)
	{
		const auto *header = reinterpret_cast<const GtrFrameHeader *>(g_frames.view);
		if (slotIndex < 0 || slotIndex >= GTR_FRAME_SLOTS)
			return false;
		const GtrFrameSlot &slot = header->Slots[slotIndex];
		const int64_t sequence = slot.Sequence;
		const int64_t frame = sequence * GTR_FRAME_SLOTS + slotIndex;
		const uint32_t width = slot.Width, height = slot.Height;
		if (frame == layer.frame || (sequence & 1) != 0 || width == 0 || height == 0 || width > header->MaxWidth || height > header->MaxHeight)
			return false;

		device *dev = runtime->get_device();
		if (width != g_width || height != g_height)
		{
			destroy_layers(dev);
			for (Layer &made : g_layers)
			{
				if (!create_texture(dev, made.color, width, height, format::r8g8b8a8_unorm) || !create_texture(dev, made.depth, width, height, format::r32_float) ||
					!create_texture(dev, made.light, GTR_LIGHT_SIZE, GTR_LIGHT_SIZE, format::r32_float))
				{
					destroy_layers(dev);
					return false;
				}
			}
			g_width = width;
			g_height = height;
			bind(runtime);
		}

		// Nothing whole until the copy is done
		layer.frame = 0;
		layer.cameraId = 0;

		// The textures rest in the state the effect samples them in, and are only writable while they are copied into
		const resource textures[2] = {layer.color.tex, layer.depth.tex};
		const resource_usage sampled[2] = {resource_usage::shader_resource, resource_usage::shader_resource};
		const resource_usage written[2] = {resource_usage::copy_dest, resource_usage::copy_dest};
		cmd_list->barrier(2, textures, sampled, written);

		// The part the guest drew in, which is all it wrote, from its place in the whole picture
		subresource_box box;
		box.left = std::min<uint32_t>(slot.RectX, width - 1);
		box.top = std::min<uint32_t>(slot.RectY, height - 1);
		box.right = std::min<uint32_t>(box.left + std::max<uint32_t>(slot.RectWidth, 1), width);
		box.bottom = std::min<uint32_t>(box.top + std::max<uint32_t>(slot.RectHeight, 1), height);
		box.front = 0;
		box.back = 1;
		const uint8_t *pixels = g_frames.view + GTR_FRAME_HEADER_BYTES + g_slotStride * slotIndex;
		const size_t layerBytes = size_t(width) * height * 4;
		const size_t first = (size_t(box.top) * width + box.left) * 4;
		subresource_data data;
		data.row_pitch = width * 4;
		data.slice_pitch = width * 4 * (box.bottom - box.top);
		data.data = const_cast<uint8_t *>(pixels + first);
		dev->update_texture_region(data, layer.color.tex, 0, &box);
		data.data = const_cast<uint8_t *>(pixels + layerBytes + first);
		dev->update_texture_region(data, layer.depth.tex, 0, &box);
		layer.rect[0] = float(box.left) / float(width);
		layer.rect[1] = float(box.top) / float(height);
		layer.rect[2] = float(box.right) / float(width);
		layer.rect[3] = float(box.bottom) / float(height);

		// The frame's shadow map, after its interface, and the way into it from the frame's camera's space: a place there is
		// so far along the camera's right, up and forward from the camera, and the map wants it along its own three
		layer.lightTan = 0.0f;
		if (slot.LightSize == GTR_LIGHT_SIZE && slot.LightTan > 0.0f)
		{
			cmd_list->barrier(layer.light.tex, resource_usage::shader_resource, resource_usage::copy_dest);
			subresource_data map;
			map.row_pitch = GTR_LIGHT_SIZE * 4;
			map.slice_pitch = GTR_LIGHT_SIZE * GTR_LIGHT_SIZE * 4;
			map.data = const_cast<uint8_t *>(pixels + layerBytes * 2 + size_t(slot.GuiWidth) * slot.GuiHeight * 4);
			dev->update_texture_region(map, layer.light.tex, 0);
			cmd_list->barrier(layer.light.tex, resource_usage::copy_dest, resource_usage::shader_resource);
			const float *cameraAxes[3] = {slot.CameraRight, slot.CameraUp, slot.CameraForward};
			const float *lightAxes[3] = {slot.LightRight, slot.LightUp, slot.LightForward};
			const double apart[3] = {slot.CameraPosition[0] - slot.LightPosition[0], slot.CameraPosition[1] - slot.LightPosition[1],
				slot.CameraPosition[2] - slot.LightPosition[2]};
			for (int row = 0; row < 3; ++row)
			{
				const float *along = lightAxes[row];
				for (int column = 0; column < 3; ++column)
					layer.lightRows[row][column] = along[0] * cameraAxes[column][0] + along[1] * cameraAxes[column][1] + along[2] * cameraAxes[column][2];
				layer.lightRows[row][3] = float(along[0] * apart[0] + along[1] * apart[1] + along[2] * apart[2]);
			}
			layer.lightTan = slot.LightTan;
		}

		cmd_list->barrier(2, textures, written, sampled);

		// The guest rewrote the slot mid-copy: what was uploaded is torn, so it isn't counted and is copied again next time
		if (slot.Sequence != sequence)
			return false;
		layer.frame = frame;
		layer.cameraId = slot.CameraId;
		g_shownAt = GetTickCount();
		g_hasFrame = true;
		return true;
	}

	void destroy_gui(device *dev)
	{
		if (g_gui.srv.handle != 0)
			dev->destroy_resource_view(g_gui.srv);
		if (g_gui.tex.handle != 0)
			dev->destroy_resource(g_gui.tex);
		g_gui = Texture();
		g_guiWidth = g_guiHeight = 0;
		g_guiFrame = 0;
		g_guiVersion = 0;
		g_hasGui = false;
	}

	/// Copies the newest frame's interface into its texture, unless it is there already.
	void update_gui(effect_runtime *runtime, command_list *cmd_list)
	{
		const auto *header = reinterpret_cast<const GtrFrameHeader *>(g_frames.view);
		const int32_t slotIndex = header->LatestSlot;
		if (slotIndex < 0 || slotIndex >= GTR_FRAME_SLOTS)
			return;
		const GtrFrameSlot &slot = header->Slots[slotIndex];
		const int64_t sequence = slot.Sequence;
		const int64_t frame = sequence * GTR_FRAME_SLOTS + slotIndex;
		const uint32_t width = slot.GuiWidth, height = slot.GuiHeight, blueFirst = slot.GuiBlueFirst;
		if (frame == g_guiFrame || (sequence & 1) != 0)
			return;
		if (width == 0 || height == 0 || width > header->MaxWidth || height > header->MaxHeight || slot.Width == 0 || slot.Height == 0)
		{
			g_hasGui = false;
			return;
		}

		device *dev = runtime->get_device();
		if (width != g_guiWidth || height != g_guiHeight || blueFirst != g_guiBlueFirst)
		{
			destroy_gui(dev);
			if (!create_texture(dev, g_gui, width, height, blueFirst != 0 ? format::b8g8r8a8_unorm : format::r8g8b8a8_unorm))
			{
				destroy_gui(dev);
				return;
			}
			g_guiWidth = width;
			g_guiHeight = height;
			g_guiBlueFirst = blueFirst;
			runtime->update_texture_bindings("GTRGUI", g_gui.srv, g_gui.srv);
		}

		// The same picture as the texture holds already: 8 MB not copied on the game's render thread
		const uint64_t version = slot.GuiVersion;
		if (version != 0 && version == g_guiVersion)
		{
			g_guiFrame = frame;
			g_hasGui = true;
			return;
		}
		cmd_list->barrier(g_gui.tex, resource_usage::shader_resource, resource_usage::copy_dest);
		subresource_data data;
		data.row_pitch = width * 4;
		data.slice_pitch = width * height * 4;
		// After the frame's colour and depth
		data.data = const_cast<uint8_t *>(g_frames.view + GTR_FRAME_HEADER_BYTES + g_slotStride * slotIndex + size_t(slot.Width) * slot.Height * 8);
		dev->update_texture_region(data, g_gui.tex, 0);
		cmd_list->barrier(g_gui.tex, resource_usage::copy_dest, resource_usage::shader_resource);
		if (slot.Sequence != sequence)
		{
			g_guiVersion = 0;
			return;
		}
		g_guiFrame = frame;
		g_guiVersion = version;
		g_hasGui = true;
	}

	/// Brings the layers up to date and says how the effect is to choose among them: by the mark in the picture (true) or
	/// the first always. tickLayer is the layer that holds each tick's frame, and latest the layer of the newest tick, for a
	/// picture whose mark can't be read.
	bool update_layers(effect_runtime *runtime, command_list *cmd_list, const GtrHostState *host, int &latest, int tickLayer[GTR_HOST_TICKS])
	{
		const auto *header = reinterpret_cast<const GtrFrameHeader *>(g_frames.view);
		latest = 0;
		std::fill(tickLayer, tickLayer + GTR_HOST_TICKS, 0);
		const bool marked = host != nullptr && std::any_of(std::begin(host->TickCamera), std::end(host->TickCamera), [](int64_t id) { return id != 0; });
		if (!marked)
		{
			upload(runtime, cmd_list, g_layers[0], header->LatestSlot);
			return false;
		}

		const auto wanted_by_a_tick = [host](int64_t cameraId) {
			return cameraId != 0 && std::find(std::begin(host->TickCamera), std::end(host->TickCamera), cameraId) != std::end(host->TickCamera);
		};
		int uploads = 0;
		int64_t newest = 0;
		bool found[GTR_HOST_TICKS] = {};
		for (int tick = 0; tick < GTR_HOST_TICKS; ++tick)
		{
			const int64_t wanted = host->TickCamera[tick];
			if (wanted == 0)
				continue;
			int layer = -1;
			for (int i = 0; i < GTR_HOST_TICKS && layer < 0; ++i)
				if (g_layers[i].cameraId == wanted)
					layer = i;
			if (layer < 0 && uploads < kUploadsPerPresent)
			{
				// Not held yet: copied, if it is still in the ring, into a layer no tick wants any more
				for (int32_t slot = 0; slot < GTR_FRAME_SLOTS; ++slot)
				{
					if (header->Slots[slot].CameraId != wanted || (header->Slots[slot].Sequence & 1) != 0)
						continue;
					for (int i = 0; i < GTR_HOST_TICKS; ++i)
					{
						if (wanted_by_a_tick(g_layers[i].cameraId))
							continue;
						++uploads;
						if (upload(runtime, cmd_list, g_layers[i], slot) && g_layers[i].cameraId == wanted)
							layer = i;
						break;
					}
					break;
				}
			}
			if (layer < 0)
				continue;
			found[tick] = true;
			tickLayer[tick] = layer;
			if (wanted > newest)
			{
				newest = wanted;
				latest = layer;
			}
		}
		// A tick whose frame has left the ring, or is yet to be copied, shows the newest there is: an older or newer picture
		// of the guest beats none
		for (int tick = 0; tick < GTR_HOST_TICKS; ++tick)
			if (!found[tick])
				tickLayer[tick] = latest;
		return true;
	}

	double now_milliseconds()
	{
		static const double perMillisecond = [] {
			LARGE_INTEGER frequency;
			QueryPerformanceFrequency(&frequency);
			return double(frequency.QuadPart) / 1000.0;
		}();
		LARGE_INTEGER counter;
		QueryPerformanceCounter(&counter);
		return double(counter.QuadPart) / perMillisecond;
	}

	/// Holds the host's frame until the guest has published one since the last, so that the host, which has the focus and
	/// with it the GPU, leaves the guest the time it needs to draw. A guest that keeps up is never waited for.
	void wait_for_guest(const GtrHostState *host)
	{
		const auto *header = reinterpret_cast<const GtrFrameHeader *>(g_frames.view);
		// The guest writes this while it is being read
		const auto published = [header] { return *reinterpret_cast<const volatile int64_t *>(&header->Published); };
		const double start = now_milliseconds();
		if (published() != g_seenPublished)
		{
			g_seenPublished = published();
			g_publishedAt = start;
			return;
		}
		if (host != nullptr && host->FreeRunning != 0)
			return;
		if (g_published == nullptr)
			g_published = OpenEventA(SYNCHRONIZE, FALSE, GTR_FRAME_PUBLISHED_EVENT);
		if (g_published == nullptr || start - g_publishedAt > kGuestStalledMilliseconds)
			return;
		// The event can be left set by a frame already counted, so the count is what is waited for
		for (double waited = 0.0; published() == g_seenPublished && waited < kGuestWaitMilliseconds; waited = now_milliseconds() - start)
			WaitForSingleObject(g_published, DWORD(kGuestWaitMilliseconds - waited) + 1);
		if (published() != g_seenPublished)
		{
			g_seenPublished = published();
			g_publishedAt = now_milliseconds();
		}
	}

	void set_uniform(effect_runtime *runtime, const char *name, bool value)
	{
		if (const effect_uniform_variable variable = runtime->find_uniform_variable(kEffect, name); variable.handle != 0)
			runtime->set_uniform_value_bool(variable, value);
	}

	void set_uniform(effect_runtime *runtime, const char *name, int value)
	{
		if (const effect_uniform_variable variable = runtime->find_uniform_variable(kEffect, name); variable.handle != 0)
			runtime->set_uniform_value_int(variable, value);
	}

	/// Has ReShade compile the effect again when its file has changed since it was last looked at.
	void reload_changed_effect(effect_runtime *runtime)
	{
		if (GetTickCount() - g_effectCheckedAt < kRetryMilliseconds)
			return;
		g_effectCheckedAt = GetTickCount();
		if (g_effectPath[0] == 0)
		{
			// Where install.ps1 and build.ps1 put it: beside the game's executable
			char folder[MAX_PATH] = {};
			GetModuleFileNameA(nullptr, folder, sizeof(folder));
			if (char *name = strrchr(folder, '\\'); name != nullptr)
				*name = 0;
			snprintf(g_effectPath, sizeof(g_effectPath), "%s\\reshade-shaders\\Shaders\\%s", folder, kEffect);
		}
		WIN32_FILE_ATTRIBUTE_DATA found = {};
		if (!GetFileAttributesExA(g_effectPath, GetFileExInfoStandard, &found))
			return;
		const bool first = g_effectWritten.dwLowDateTime == 0 && g_effectWritten.dwHighDateTime == 0;
		if (!first && CompareFileTime(&found.ftLastWriteTime, &g_effectWritten) != 0)
		{
			runtime->reload_effect_next_frame(kEffect);
			reshade::log::message(reshade::log::level::info, "GTR: the effect's file changed; compiling it again");
		}
		g_effectWritten = found.ftLastWriteTime;
	}

	void on_begin_effects(effect_runtime *runtime, command_list *cmd_list, resource_view, resource_view)
	{
		reload_changed_effect(runtime);
		const bool hosted = open(g_host, GTR_HOST_MAPPING_NAME, GTR_HOST_MAGIC, FILE_MAP_ALL_ACCESS, sizeof(GtrHostState));
		const auto *host = hosted ? reinterpret_cast<const GtrHostState *>(g_host.view) : nullptr;

		// A script that has stopped counting is paused (the game's pause menu stops scripts) and can't say to hide the guest
		static uint32_t heartbeat = 0;
		static DWORD heartbeatAt = 0;
		if (host != nullptr && host->Heartbeat != heartbeat)
		{
			heartbeat = host->Heartbeat;
			heartbeatAt = GetTickCount();
		}
		const bool scriptRunning = host == nullptr || GetTickCount() - heartbeatAt < kPausedMilliseconds;

		bool active = (host == nullptr || host->Active != 0) && scriptRunning && open_frames();
		bool marked = false;
		int latest = 0, tickLayer[GTR_HOST_TICKS] = {};
		if (active)
		{
			wait_for_guest(host);
			marked = update_layers(runtime, cmd_list, host, latest, tickLayer);
			update_gui(runtime, cmd_list);
		}
		active = active && g_hasFrame && GetTickCount() - g_shownAt < kStaleMilliseconds;
		if (g_frames.view != nullptr && !active && g_hasFrame && GetTickCount() - g_shownAt >= kStaleMilliseconds * 5)
		{
			// A guest that was restarted makes a new mapping under the same name; this one is dropped so the new one is found
			close(g_frames);
			g_hasFrame = false;
			g_guiFrame = 0;
			g_hasGui = false;
		}

		// Without the host's depth buffer the guest is drawn over everything, so its coming and going is worth a line in ReShade.log
		static int hadDepth = -1;
		resource_view depth = {0}, depthSrgb = {0};
		if (const effect_texture_variable variable = runtime->find_texture_variable(kEffect, "DepthBufferTex"); variable.handle != 0)
			runtime->get_texture_binding(variable, &depth, &depthSrgb);
		if (const int hasDepth = depth.handle != 0; hasDepth != hadDepth)
		{
			hadDepth = hasDepth;
			reshade::log::message(hasDepth ? reshade::log::level::info : reshade::log::level::warning,
				hasDepth ? "GTR: the host's depth buffer is available" : "GTR: ReShade has no depth buffer for the host yet; the guest cannot be hidden behind it");
		}

		set_uniform(runtime, "GtrActive", active);
		set_uniform(runtime, "GtrMarked", marked);
		set_uniform(runtime, "GtrMarkOffset", host != nullptr ? host->MarkOffset : 0);
		set_uniform(runtime, "GtrLatestLayer", latest);
		if (const effect_uniform_variable variable = runtime->find_uniform_variable(kEffect, "GtrTickLayerA"); variable.handle != 0)
			runtime->set_uniform_value_float(variable, float(tickLayer[0]), float(tickLayer[1]), float(tickLayer[2]), float(tickLayer[3]));
		if (const effect_uniform_variable variable = runtime->find_uniform_variable(kEffect, "GtrTickLayerB"); variable.handle != 0)
			runtime->set_uniform_value_float(variable, float(tickLayer[4]), float(tickLayer[5]), float(tickLayer[6]), float(tickLayer[7]));
		set_uniform(runtime, "GtrGui", active && g_hasGui);
		for (int i = 0; i < GTR_HOST_TICKS; ++i)
		{
			char name[24];
			snprintf(name, sizeof(name), "GtrLayerRect%d", i);
			if (const effect_uniform_variable variable = runtime->find_uniform_variable(kEffect, name); variable.handle != 0)
				runtime->set_uniform_value_float(variable, g_layers[i].rect[0], g_layers[i].rect[1], g_layers[i].rect[2], g_layers[i].rect[3]);
			for (int row = 0; row < 3; ++row)
			{
				snprintf(name, sizeof(name), "GtrSunRow%c%d", 'A' + row, i);
				if (const effect_uniform_variable variable = runtime->find_uniform_variable(kEffect, name); variable.handle != 0)
					runtime->set_uniform_value_float(variable, g_layers[i].lightRows[row][0], g_layers[i].lightRows[row][1], g_layers[i].lightRows[row][2], g_layers[i].lightRows[row][3]);
			}
			snprintf(name, sizeof(name), "GtrSunTan%d", i);
			if (const effect_uniform_variable variable = runtime->find_uniform_variable(kEffect, name); variable.handle != 0)
				runtime->set_uniform_value_float(variable, g_layers[i].lightTan);
		}
		// The painted people and props (GtrPaintState), straight from the script's boxes
		const bool painting = open(g_paint, GTR_PAINT_MAPPING_NAME, GTR_PAINT_MAGIC, FILE_MAP_READ, sizeof(GtrPaintState));
		const auto *paint = painting ? reinterpret_cast<const GtrPaintState *>(g_paint.view) : nullptr;
		const int painted = paint != nullptr && active ? int(std::min<uint32_t>(paint->Count, GTR_PAINT_BOXES)) : 0;
		set_uniform(runtime, "GtrPaintCount", painted);
		if (painted > 0)
			if (const effect_uniform_variable variable = runtime->find_uniform_variable(kEffect, "GtrPaint"); variable.handle != 0)
				runtime->set_uniform_value_float(variable, reinterpret_cast<const float *>(paint->Boxes), size_t(painted) * 16);
		set_uniform(runtime, "GtrShowMark", host != nullptr && host->ShowMark != 0);
		if (host != nullptr && host->DebugView != 0)
			set_uniform(runtime, "DebugView", host->DebugView - 1);
		if (const effect_uniform_variable variable = runtime->find_uniform_variable(kEffect, "GtrSunShadow"); variable.handle != 0)
			runtime->set_uniform_value_float(variable, host != nullptr ? host->SunShadow : 0.0f);
		if (host != nullptr && host->SunShadow > 0.0f)
		{
			if (const effect_uniform_variable variable = runtime->find_uniform_variable(kEffect, "GtrSunView"); variable.handle != 0)
				runtime->set_uniform_value_float(variable, host->SunView[0], host->SunView[1], host->SunView[2]);
			if (host->SunTint[0] + host->SunTint[1] + host->SunTint[2] > 0.0f)
				if (const effect_uniform_variable variable = runtime->find_uniform_variable(kEffect, "GtrSunTint"); variable.handle != 0)
					runtime->set_uniform_value_float(variable, host->SunTint[0], host->SunTint[1], host->SunTint[2]);
		}
		// The world's up is for the lamps' shadows too, which are for when the sun casts none
		if (host != nullptr && host->TanHalfFov > 0.0f)
		{
			if (const effect_uniform_variable variable = runtime->find_uniform_variable(kEffect, "GtrUpView"); variable.handle != 0)
				runtime->set_uniform_value_float(variable, host->UpView[0], host->UpView[1], host->UpView[2]);
		}
		if (host != nullptr && host->TanHalfFov > 0.0f)
		{
			if (const effect_uniform_variable variable = runtime->find_uniform_variable(kEffect, "ViewTangent"); variable.handle != 0)
				runtime->set_uniform_value_float(variable, host->TanHalfFov);
		}
		if (host != nullptr && host->NearClip > 0.0f && host->FarClip > host->NearClip)
		{
			if (const effect_uniform_variable variable = runtime->find_uniform_variable(kEffect, "HostPlanes"); variable.handle != 0)
				runtime->set_uniform_value_float(variable, host->NearClip, host->FarClip);
		}
	}

	/// Writes a picture as a 32-bit BMP, which keeps its pixels blue first.
	void save_bmp(const char *path, std::vector<uint8_t> &pixels, bool blueFirst, uint32_t width, uint32_t height)
	{
		BITMAPFILEHEADER file = {};
		BITMAPINFOHEADER info = {};
		file.bfType = 0x4D42;
		file.bfOffBits = sizeof(file) + sizeof(info);
		file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
		info.biSize = sizeof(info);
		info.biWidth = static_cast<LONG>(width);
		info.biHeight = -static_cast<LONG>(height); // rows top to bottom
		info.biPlanes = 1;
		info.biBitCount = 32;
		info.biCompression = BI_RGB;

		for (size_t i = 0; i + 3 < pixels.size(); i += 4)
		{
			if (!blueFirst)
				std::swap(pixels[i], pixels[i + 2]);
			pixels[i + 3] = 255;
		}
		FILE *out = nullptr;
		if (fopen_s(&out, path, "wb") != 0 || out == nullptr)
			return;
		fwrite(&file, sizeof(file), 1, out);
		fwrite(&info, sizeof(info), 1, out);
		fwrite(pixels.data(), 1, pixels.size(), out);
		fclose(out);
	}

	void on_finish_effects(effect_runtime *runtime, command_list *, resource_view, resource_view)
	{
		if (g_host.view == nullptr)
			return;
		// The script's request counter is read from the mapping and the answer written back into it
		auto *host = reinterpret_cast<GtrHostState *>(const_cast<uint8_t *>(g_host.view));
		const int32_t request = host->CaptureRequest;
		if (request == g_captured)
			return;
		g_captured = request;

		uint32_t width = 0, height = 0;
		runtime->get_screenshot_width_and_height(&width, &height);
		std::vector<uint8_t> pixels(size_t(width) * height * 4);
		char path[sizeof(host->CapturePath) + 1] = {};
		std::memcpy(path, host->CapturePath, sizeof(host->CapturePath));
		// The picture comes back in the order the game's back buffer keeps its channels in, which GTA V's is blue first.
		// (Seen in GTA V Legacy: pictures saved as if red came first had fire burning blue.)
		const format backBuffer = format_to_default_typed(runtime->get_device()->get_resource_desc(runtime->get_back_buffer(0)).texture.format);
		const bool blueFirst = backBuffer == format::b8g8r8a8_unorm || backBuffer == format::b8g8r8x8_unorm;
		if (width != 0 && height != 0 && path[0] != 0 && runtime->capture_screenshot(pixels.data()))
			save_bmp(path, pixels, blueFirst, width, height);
		host->CaptureDone = request;
	}

	void on_reloaded_effects(effect_runtime *runtime)
	{
		if (g_width != 0)
			bind(runtime);
		if (g_guiWidth != 0)
			runtime->update_texture_bindings("GTRGUI", g_gui.srv, g_gui.srv);
	}

	void on_destroy_effect_runtime(effect_runtime *runtime)
	{
		destroy_layers(runtime->get_device());
		destroy_gui(runtime->get_device());
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(module))
			return FALSE;
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish_effects);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		break;
	case DLL_PROCESS_DETACH:
		reshade::unregister_addon(module);
		close(g_frames);
		close(g_host);
		close(g_paint);
		break;
	}
	return TRUE;
}
