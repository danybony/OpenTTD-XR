/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/**
 * @file xr_panels.cpp Windows floating in space as panels, around the tabletop view.
 *
 * All windows keep drawing to the one video buffer as usual, but that buffer is taller than the
 * screen, and the windows are laid out in the extra part so that none overlaps another, like an
 * atlas. The main view has the part that is the screen to itself. Each window is then shown as a
 * panel that samples its part of the buffer, and the cursor is translated from the screen to that
 * part of the buffer. So the windows themselves do not know: they draw and take clicks as always.
 */

#include "stdafx.h"
#include "xr_panels.h"

#ifdef WITH_3D_PROOF

#include "gfx_func.h"
#include "tile_type.h"
#include "viewport_type.h"
#include "widget_type.h"
#include "window_func.h"
#include "window_gui.h"
#include "video/proof3d.h"
#include "video/video_driver.hpp"
#include "video/xr_panel_math.h"

#include "safeguards.h"

using Terrain3D::Vec;

/** Distance from the eye of panels that have not been placed in the room. Their size on the screen does not depend on it. */
static constexpr float PANEL_DISTANCE = 400;
/** How tall the video buffer is, with room for the windows below the screen. All OpenGL ES 3.0 hardware of interest allows it. */
static constexpr int ATLAS_HEIGHT = 4096;

struct Panel {
	Window *window;
	float screen_x, screen_y; ///< Middle of the panel on the screen, in pixels.
	Vec room; ///< Where the panel is in the room, if anchored there.
	std::array<std::array<float, 2>, 4> on_screen; ///< Corners on the screen, with the grip: top left, top right, bottom right, bottom left.
	bool visible; ///< Whether all of it is in front of the eye.
	int grip_height; ///< Height of the handle to drag the panel by, if it has no title bar to do that.
	bool grip_above; ///< Whether the handle is above the window rather than below it.
};

static bool _active = false;
static bool _room_anchored = false;
static std::vector<Panel> _panels; ///< From the back to the front.
static std::vector<XRPanelDraw> _draws;
static Panel *_dragging = nullptr;
static int _display_height = 0; ///< Height of the part of the buffer that is the screen.
static Point _drag_offset{};

/** The camera of the view the panels float in. */
struct PanelCamera {
	Terrain3D::Camera camera;
	Vec eye, forward, right, up;
	float units_per_pixel; ///< Size of a pixel at #PANEL_DISTANCE.
};

static PanelCamera GetPanelCamera(const Viewport &vp)
{
	constexpr float DEGREES = 3.14159265359f / 180;
	const auto &e = vp.camera_eye;
	const float width = static_cast<float>(vp.width), height = static_cast<float>(vp.height);
	PanelCamera c{Terrain3D::PerspectiveCamera({e[0], e[1], e[2]}, e[3], e[4], e[5], width, height, 1024 * TILE_SIZE), {e[0], e[1], e[2]}, {}, {}, {}, 0};
	const auto ground = Terrain3D::HeadingForward(e[3]);
	const float pitch = e[4] * DEGREES;
	c.forward = {ground[0] * std::cos(pitch), ground[1] * std::cos(pitch), std::sin(pitch)};
	c.right = {ground[1], -ground[0], 0};
	c.up = {c.right.y * c.forward.z, -c.right.x * c.forward.z, c.right.x * c.forward.y - c.right.y * c.forward.x};
	c.units_per_pixel = 2 * PANEL_DISTANCE * std::tan(e[5] * DEGREES / 2) / height;
	return c;
}

/** Where in the world a place on the screen is, at the distance of the panels. */
static Vec PanelWorldPosition(const PanelCamera &c, const Viewport &vp, float screen_x, float screen_y)
{
	const float across = (screen_x - vp.width / 2.0f) * c.units_per_pixel, along = (vp.height / 2.0f - screen_y) * c.units_per_pixel;
	return {c.eye.x + c.forward.x * PANEL_DISTANCE + c.right.x * across + c.up.x * along,
		c.eye.y + c.forward.y * PANEL_DISTANCE + c.right.y * across + c.up.y * along,
		c.eye.z + c.forward.z * PANEL_DISTANCE + c.right.z * across + c.up.z * along};
}

bool IsXRPanelsActive()
{
	return _active;
}

int GetXRAtlasExtraHeight()
{
	return _active ? std::max(ATLAS_HEIGHT - _display_height, 0) : 0;
}

int GetXRDisplayHeight()
{
	return _display_height;
}

static void MoveWindowTo(Window *w, int left, int top);

void SetXRPanels(bool active)
{
	if (_active == active) return;
	/* Leaving, windows stay where their panels were on the screen, as far as the game's own limits allow. */
	if (!active) {
		for (const auto &panel : _panels) {
			MoveWindowTo(panel.window, static_cast<int>(std::round(panel.screen_x - panel.window->width / 2.0f)), static_cast<int>(std::round(panel.screen_y - panel.window->height / 2.0f)));
		}
	}
	_panels.clear();
	_draws.clear();
	_dragging = nullptr;
	_display_height = active ? _screen.height : 0;
	_active = active;
	/* The buffer changes size, which also makes all windows find their place again. */
	VideoDriver::GetInstance()->VideoBufferLayoutChanged();
	ForgetProof3DImages();
	MarkWholeScreenDirty();
}

void SetXRPanelsRoomAnchored(bool room)
{
	_room_anchored = room;
	const Window *main = GetMainWindow();
	if (main == nullptr || main->viewport == nullptr) return;
	const auto c = GetPanelCamera(*main->viewport);
	for (auto &panel : _panels) panel.room = PanelWorldPosition(c, *main->viewport, panel.screen_x, panel.screen_y);
}

static Panel *FindPanel(const Window *w)
{
	auto it = std::find_if(_panels.begin(), _panels.end(), [&](const Panel &p) { return p.window == w; });
	return it == _panels.end() ? nullptr : &*it;
}

/** Lay out all windows in rows in the part of the buffer below the screen, moving those that are not in place yet. */
static void MoveWindowTo(Window *w, int left, int top)
{
	if (w->left == left && w->top == top) return;
	w->SetDirty();
	if (w->viewport != nullptr) {
		w->viewport->left += left - w->left;
		w->viewport->top += top - w->top;
	}
	w->left = left;
	w->top = top;
	w->SetDirty();
}

static void LayoutAtlas()
{
	int x = 0, y = _display_height, row = 0;
	for (Window *w : Window::IterateFromBack()) {
		if (w->window_class == WindowClass::MainWindow) continue;
		if (x + w->width > _screen.width) {
			x = 0;
			y += row;
			row = 0;
		}
		/* When even this is full, later windows overlap the first ones rather than disappear. */
		MoveWindowTo(w, x, y + w->height > _screen.height ? _display_height : y);
		x += w->width;
		row = std::max(row, w->height);
	}
}

void UpdateXRPanels()
{
	if (!_active) return;
	Window *main = GetMainWindow();
	if (main == nullptr || main->viewport == nullptr || !IsTerrain3DTabletop(*main->viewport)) {
		SetXRPanels(false);
		return;
	}

	/* Keep the panels of windows that are still there, in the order of the windows. A new one appears where
	 * the game opened it, or next to the panel of its parent, which is where in the buffer it was opened. */
	std::vector<Panel> panels;
	const Window *dragged = _dragging != nullptr ? _dragging->window : nullptr;
	for (Window *w : Window::IterateFromBack()) {
		if (w->window_class == WindowClass::MainWindow) continue;
		if (Panel *known = FindPanel(w); known != nullptr) {
			panels.push_back(*known);
			continue;
		}
		/* The whole buffer is free where the game looks for a place to open a window, so that says little. Bars
		 * have their fixed places, fitted to the height of the screen, and the others open in a cascade. */
		float x = w->left + w->width / 2.0f;
		float y = w->top * std::max(_display_height - w->height, 0) / static_cast<float>(std::max(_screen.height - w->height, 1)) + w->height / 2.0f;
		const bool bar = w->window_class == WindowClass::MainToolbar || w->window_class == WindowClass::Statusbar;
		if (!bar) {
			static int cascade = 0;
			const int step = 24 * _gui_scale / 100;
			cascade = (cascade + 1) % 8;
			x = std::max(_screen.width / 4.0f + cascade * step, w->width / 2.0f);
			y = std::clamp(_display_height / 6.0f + cascade * step + w->height / 2.0f, w->height / 2.0f, std::max(_display_height - w->height / 2.0f, w->height / 2.0f));
		}
		if (const Panel *parent = FindPanel(w->parent); parent != nullptr) {
			x = w->left + w->width / 2.0f + parent->screen_x - (w->parent->left + w->parent->width / 2.0f);
			y = w->top + w->height / 2.0f + parent->screen_y - (w->parent->top + w->parent->height / 2.0f);
		}
		const auto c = GetPanelCamera(*main->viewport);
		/* Windows without a title bar get a handle, on the side where there is room. */
		panels.push_back({w, x, y, PanelWorldPosition(c, *main->viewport, x, y), {}, true, bar ? 20 * _gui_scale / 100 : 0, y > _display_height / 2.0f});
	}
	_panels = std::move(panels);
	_dragging = nullptr;
	if (dragged != nullptr) _dragging = FindPanel(dragged);

	LayoutAtlas();

	const auto c = GetPanelCamera(*main->viewport);
	const Viewport &vp = *main->viewport;
	_draws.clear();
	const float buffer_width = static_cast<float>(_screen.width), buffer_height = static_cast<float>(_screen.height);
	for (auto &panel : _panels) {
		const Window *w = panel.window;
		const int total_height = w->height + panel.grip_height;
		std::array<Vec, 4> world;
		if (_room_anchored) {
			world = XRPanel::BillboardCorners(panel.room, c.eye, w->width * c.units_per_pixel, total_height * c.units_per_pixel);
		} else {
			/* Lined up with the screen and on whole pixels, so text stays sharp. */
			const float left = std::round(panel.screen_x - w->width / 2.0f);
			const float top = std::round(panel.screen_y - w->height / 2.0f) - (panel.grip_above ? panel.grip_height : 0);
			world = {PanelWorldPosition(c, vp, left, top), PanelWorldPosition(c, vp, left + w->width, top),
				PanelWorldPosition(c, vp, left + w->width, top + total_height), PanelWorldPosition(c, vp, left, top + total_height)};
		}
		/* A part of the panel, in fractions of its width and height. */
		auto clip_at = [&](float u, float v) {
			const Vec p{world[0].x + (world[1].x - world[0].x) * u + (world[3].x - world[0].x) * v,
				world[0].y + (world[1].y - world[0].y) * u + (world[3].y - world[0].y) * v,
				world[0].z + (world[1].z - world[0].z) * u + (world[3].z - world[0].z) * v};
			std::array<float, 4> clip;
			for (int row = 0; row < 4; row++) {
				const auto r = c.camera.Row(row);
				clip[row] = r[0] * p.x + r[1] * p.y + r[2] * p.z + r[3];
			}
			return clip;
		};
		panel.visible = true;
		const std::array<std::array<float, 2>, 4> fractions{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
		for (int i = 0; i < 4; i++) {
			const auto clip = clip_at(fractions[i][0], fractions[i][1]);
			if (clip[3] <= 0.01f) {
				panel.visible = false;
				break;
			}
			panel.on_screen[i] = {(clip[0] / clip[3] + 1) * vp.width / 2 + vp.left, (1 - clip[1] / clip[3]) * vp.height / 2 + vp.top};
		}
		if (!panel.visible) continue;

		const float grip_part = static_cast<float>(panel.grip_height) / total_height;
		const float window_top = panel.grip_above ? grip_part : 0, window_bottom = panel.grip_above ? 1 : 1 - grip_part;
		/* The order of the corners of a draw is a triangle strip. */
		_draws.push_back({{clip_at(0, window_top), clip_at(1, window_top), clip_at(0, window_bottom), clip_at(1, window_bottom)},
			{w->left / buffer_width, w->top / buffer_height, (w->left + w->width) / buffer_width, (w->top + w->height) / buffer_height},
			w->viewport != nullptr, false});
		if (panel.grip_height != 0) {
			/* A pill in the middle of the strip, which as a whole can be grabbed. */
			const float grip_top = panel.grip_above ? grip_part * 0.25f : window_bottom + grip_part * 0.25f, grip_bottom = grip_top + grip_part * 0.5f;
			_draws.push_back({{clip_at(0.35f, grip_top), clip_at(0.65f, grip_top), clip_at(0.35f, grip_bottom), clip_at(0.65f, grip_bottom)}, {}, false, true});
		}
	}
}

void LogXRPanels()
{
	Debug(Facility::Driver, Severity::Error, "PANELS screen={}x{} display={} count={}", _screen.width, _screen.height, _display_height, _panels.size());
	for (const auto &panel : _panels) {
		const Window *w = panel.window;
		Debug(Facility::Driver, Severity::Error, "PANEL class={} rect={},{} {}x{} screen={},{} visible={}", to_underlying(w->window_class), w->left, w->top, w->width, w->height, panel.screen_x, panel.screen_y, panel.visible);
	}
}

const std::vector<XRPanelDraw> &GetXRPanelDraws()
{
	return _draws;
}

/** Find the panel at a place on the screen, from the front to the back, and where on the window that is. */
static Panel *FindPanelAt(int x, int y, Point &in_buffer, bool &on_grip)
{
	for (auto it = _panels.rbegin(); it != _panels.rend(); ++it) {
		if (!it->visible) continue;
		float u, v;
		if (!XRPanel::ScreenToPanel(it->on_screen, static_cast<float>(x), static_cast<float>(y), u, v)) continue;
		const Window *w = it->window;
		const int total_height = w->height + it->grip_height;
		const int along = std::min(static_cast<int>(v * total_height), total_height - 1) - (it->grip_above ? it->grip_height : 0);
		on_grip = along < 0 || along >= w->height;
		in_buffer = {w->left + std::min(static_cast<int>(u * w->width), w->width - 1), w->top + std::clamp(along, 0, w->height - 1)};
		return &*it;
	}
	return nullptr;
}

Point MapScreenToXRPanels(int x, int y)
{
	if (!_active) return {x, y};
	Point in_buffer;
	bool on_grip;
	/* Anything that is not on a panel is the main view, which is the part of the buffer that is the screen. */
	return FindPanelAt(x, y, in_buffer, on_grip) != nullptr ? in_buffer : Point{x, y};
}

bool StartXRPanelDrag(int x, int y)
{
	if (!_active) return false;
	Point in_buffer;
	bool on_grip;
	Panel *panel = FindPanelAt(x, y, in_buffer, on_grip);
	if (panel == nullptr) return false;
	if (!on_grip) {
		if (panel->window->nested_root == nullptr) return false;
		const NWidgetCore *widget = panel->window->nested_root->GetWidgetFromPos(in_buffer.x - panel->window->left, in_buffer.y - panel->window->top);
		if (widget == nullptr || widget->type != WWT_CAPTION) return false;
	}
	_dragging = panel;
	_drag_offset = {x, y};
	return true;
}

bool IsXRPanelDragging()
{
	return _dragging != nullptr;
}

void MoveXRPanelDrag(int x, int y)
{
	if (_dragging == nullptr) return;
	_dragging->screen_x += x - _drag_offset.x;
	_dragging->screen_y += y - _drag_offset.y;
	_drag_offset = {x, y};
	if (const Window *main = GetMainWindow(); _room_anchored && main != nullptr && main->viewport != nullptr) {
		_dragging->room = PanelWorldPosition(GetPanelCamera(*main->viewport), *main->viewport, _dragging->screen_x, _dragging->screen_y);
	}
}

void EndXRPanelDrag()
{
	_dragging = nullptr;
}

#endif /* WITH_3D_PROOF */
