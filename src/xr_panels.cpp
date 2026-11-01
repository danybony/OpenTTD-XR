/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/**
 * @file xr_panels.cpp Windows floating in space as panels, around the tabletop view.
 *
 * All windows keep drawing to the one video buffer as usual, but are laid out there so that
 * none overlaps another, like an atlas. Each is then shown as a panel that samples its part
 * of the buffer, and the cursor is translated from the screen to that part of the buffer. So
 * the windows themselves do not know: they draw and take clicks as always.
 */

#include "stdafx.h"
#include "xr_panels.h"

bool _xr_cursor_on_world = false;

#ifdef WITH_3D_PROOF

#include "gfx_func.h"
#include "tile_type.h"
#include "viewport_type.h"
#include "widget_type.h"
#include "window_func.h"
#include "window_gui.h"
#include "video/proof3d.h"
#include "video/xr_panel_math.h"

#include "safeguards.h"

using Terrain3D::Vec;

/** Distance from the eye of panels that have not been placed in the room. Their size on the screen does not depend on it. */
static constexpr float PANEL_DISTANCE = 400;

struct Panel {
	Window *window;
	float screen_x, screen_y; ///< Middle of the panel on the screen, in pixels.
	Vec room; ///< Where the panel is in the room, if anchored there.
	std::array<std::array<float, 2>, 4> on_screen; ///< Corners on the screen: top left, top right, bottom right, bottom left.
	bool visible; ///< Whether all of it is in front of the eye.
};

static bool _active = false;
static bool _room_anchored = false;
static std::vector<Panel> _panels; ///< From the back to the front.
static std::vector<XRPanelDraw> _draws;
static Panel *_dragging = nullptr;
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

void SetXRPanels(bool active)
{
	if (_active == active) return;
	_active = active;
	_panels.clear();
	_draws.clear();
	_dragging = nullptr;
	_xr_cursor_on_world = false;
	if (!active) RelocateAllWindows(_screen.width, _screen.height);
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

/** Lay out all windows in rows in the buffer, moving those that are not in place yet. */
static void LayoutAtlas()
{
	int x = 0, y = 0, row = 0;
	for (Window *w : Window::IterateFromBack()) {
		if (w->window_class == WindowClass::MainWindow) continue;
		if (x + w->width > _screen.width) {
			x = 0;
			y += row;
			row = 0;
		}
		/* The buffer is the size of the screen for now. Later windows overlap the first ones when it is full. */
		const int top = y + w->height > _screen.height ? 0 : y;
		if (w->left != x || w->top != top) {
			w->SetDirty();
			w->left = x;
			w->top = top;
			w->SetDirty();
		}
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
		float x = w->left + w->width / 2.0f, y = w->top + w->height / 2.0f;
		if (const Panel *parent = FindPanel(w->parent); parent != nullptr) {
			x += parent->screen_x - (w->parent->left + w->parent->width / 2.0f);
			y += parent->screen_y - (w->parent->top + w->parent->height / 2.0f);
		}
		const auto c = GetPanelCamera(*main->viewport);
		panels.push_back({w, x, y, PanelWorldPosition(c, *main->viewport, x, y), {}, true});
	}
	_panels = std::move(panels);
	_dragging = nullptr;
	if (dragged != nullptr) _dragging = FindPanel(dragged);

	LayoutAtlas();

	const auto c = GetPanelCamera(*main->viewport);
	const Viewport &vp = *main->viewport;
	_draws.clear();
	for (auto &panel : _panels) {
		const Window *w = panel.window;
		const float panel_width = w->width * c.units_per_pixel, panel_height = w->height * c.units_per_pixel;
		std::array<Vec, 4> world;
		if (_room_anchored) {
			world = XRPanel::BillboardCorners(panel.room, c.eye, panel_width, panel_height);
		} else {
			/* Lined up with the screen and on whole pixels, so text stays sharp. */
			const float left = std::round(panel.screen_x - w->width / 2.0f), top = std::round(panel.screen_y - w->height / 2.0f);
			world = {PanelWorldPosition(c, vp, left, top), PanelWorldPosition(c, vp, left + w->width, top),
				PanelWorldPosition(c, vp, left + w->width, top + w->height), PanelWorldPosition(c, vp, left, top + w->height)};
		}
		std::array<std::array<float, 4>, 4> clip;
		panel.visible = true;
		for (int i = 0; i < 4; i++) {
			for (int row = 0; row < 4; row++) {
				const auto r = c.camera.Row(row);
				clip[i][row] = r[0] * world[i].x + r[1] * world[i].y + r[2] * world[i].z + r[3];
			}
			if (clip[i][3] <= 0.01f) {
				panel.visible = false;
				continue;
			}
			panel.on_screen[i] = {(clip[i][0] / clip[i][3] + 1) * vp.width / 2 + vp.left, (1 - clip[i][1] / clip[i][3]) * vp.height / 2 + vp.top};
		}
		if (!panel.visible) continue;
		const float width = static_cast<float>(_screen.width), height = static_cast<float>(_screen.height);
		/* The draw order of the corners is a triangle strip. */
		_draws.push_back({{clip[0], clip[1], clip[3], clip[2]}, {w->left / width, w->top / height, (w->left + w->width) / width, (w->top + w->height) / height}});
	}
}

const std::vector<XRPanelDraw> &GetXRPanelDraws()
{
	return _draws;
}

/** Find the panel at a place on the screen, from the front to the back, and where on the window that is. */
static Panel *FindPanelAt(int x, int y, Point &in_buffer)
{
	for (auto it = _panels.rbegin(); it != _panels.rend(); ++it) {
		if (!it->visible) continue;
		float u, v;
		if (!XRPanel::ScreenToPanel(it->on_screen, static_cast<float>(x), static_cast<float>(y), u, v)) continue;
		const Window *w = it->window;
		in_buffer = {w->left + std::min(static_cast<int>(u * w->width), w->width - 1), w->top + std::min(static_cast<int>(v * w->height), w->height - 1)};
		return &*it;
	}
	return nullptr;
}

Point MapScreenToXRPanels(int x, int y)
{
	if (!_active) return {x, y};
	Point in_buffer;
	_xr_cursor_on_world = FindPanelAt(x, y, in_buffer) == nullptr;
	return _xr_cursor_on_world ? Point{x, y} : in_buffer;
}

bool StartXRPanelDrag(int x, int y)
{
	if (!_active) return false;
	Point in_buffer;
	Panel *panel = FindPanelAt(x, y, in_buffer);
	if (panel == nullptr || panel->window->nested_root == nullptr) return false;
	const NWidgetCore *widget = panel->window->nested_root->GetWidgetFromPos(in_buffer.x - panel->window->left, in_buffer.y - panel->window->top);
	if (widget == nullptr || widget->type != WWT_CAPTION) return false;
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
