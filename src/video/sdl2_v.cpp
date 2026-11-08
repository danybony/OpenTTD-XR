/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file sdl2_v.cpp Implementation of the SDL2 video driver. */

#include "../stdafx.h"
#include "../openttd.h"
#include "../gfx_func.h"
#include "../blitter/factory.hpp"
#include "../thread.h"
#include "../progress.h"
#include "../core/math_func.hpp"
#include "../core/geometry_func.hpp"
#include "../core/utf8.hpp"
#include "../fileio_func.h"
#include "../framerate_type.h"
#include "../window_func.h"
#include "../viewport_func.h"
#include "sdl2_v.h"
#include "touch_gesture.h"
#include "../xr_panels.h"
#include "../xr_test_log.h"
#include <SDL.h>
#ifdef __EMSCRIPTEN__
#	include <emscripten.h>
#	include <emscripten/html5.h>
#endif

#include "../safeguards.h"

void VideoDriver_SDL_Base::MakeDirty(int left, int top, int width, int height)
{
	Rect r = {left, top, left + width, top + height};
	this->dirty_rect = BoundingRect(this->dirty_rect, r);
}

void VideoDriver_SDL_Base::CheckPaletteAnim()
{
	if (!CopyPalette(this->local_palette)) return;
	this->MakeDirty(0, 0, _screen.width, _screen.height);
}

/** Set of common resolutions that can be used as default when none is provided by SDL. */
static const Dimension _default_resolutions[] = {
	{  640,  480 },
	{  800,  600 },
	{ 1024,  768 },
	{ 1152,  864 },
	{ 1280,  800 },
	{ 1280,  960 },
	{ 1280, 1024 },
	{ 1400, 1050 },
	{ 1600, 1200 },
	{ 1680, 1050 },
	{ 1920, 1200 }
};

/** Find and store all possible resolutions into \c _resolutions. */
static void FindResolutions()
{
	_resolutions.clear();

	for (int display = 0; display < SDL_GetNumVideoDisplays(); display++) {
		for (int i = 0; i < SDL_GetNumDisplayModes(display); i++) {
			SDL_DisplayMode mode;
			SDL_GetDisplayMode(display, i, &mode);

			if (mode.w < 640 || mode.h < 480) continue;
			if (std::ranges::find(_resolutions, Dimension(mode.w, mode.h)) != _resolutions.end()) continue;
			_resolutions.emplace_back(mode.w, mode.h);
		}
	}

	/* We have found no resolutions, show the default list */
	if (_resolutions.empty()) {
		_resolutions.assign(std::begin(_default_resolutions), std::end(_default_resolutions));
	}

	SortResolutions();
}

/**
 * Get the best resolution given the requested width and height.
 * @param[inout] w The requested width, updated with the best fit.
 * @param[inout] h The requested height, updated with the best fit.
 */
static void GetAvailableVideoMode(uint *w, uint *h)
{
	/* All modes available? */
	if (!_fullscreen || _resolutions.empty()) return;

	/* Is the wanted mode among the available modes? */
	if (std::ranges::find(_resolutions, Dimension(*w, *h)) != _resolutions.end()) return;

	/* Use the closest possible resolution */
	uint best = 0;
	uint delta = Delta(_resolutions[0].width, *w) * Delta(_resolutions[0].height, *h);
	for (uint i = 1; i != _resolutions.size(); ++i) {
		uint newdelta = Delta(_resolutions[i].width, *w) * Delta(_resolutions[i].height, *h);
		if (newdelta < delta) {
			best = i;
			delta = newdelta;
		}
	}
	*w = _resolutions[best].width;
	*h = _resolutions[best].height;
}

/**
 * Find the display we want to start OpenTTD on.
 * @param startup_display The user preferred display, MAX_UINT32 when not given.
 * @return The best display to start OpenTTD on.
 */
static uint FindStartupDisplay(uint startup_display)
{
	int num_displays = SDL_GetNumVideoDisplays();

	/* If the user indicated a valid monitor, use that. */
	if (IsInsideBS(startup_display, 0, num_displays)) return startup_display;

	/* Mouse position decides which display to use. */
	int mx, my;
	SDL_GetGlobalMouseState(&mx, &my);
	for (int display = 0; display < num_displays; ++display) {
		SDL_Rect r;
		if (SDL_GetDisplayBounds(display, &r) == 0 && IsInsideBS(mx, r.x, r.w) && IsInsideBS(my, r.y, r.h)) {
			Debug(Facility::Driver, Severity::Error, "SDL2: Mouse is at ({}, {}), use display {} ({}, {}, {}, {})", mx, my, display, r.x, r.y, r.w, r.h);
			return display;
		}
	}

	return 0;
}

/**
 * Indicate to the driver the client-size might have changed.
 * @param w The new width of the window.
 * @param h The new height of the window.
 * @param force Whether to force full reallocation, instead of not reallocating when size did not change.
 */
void VideoDriver_SDL_Base::VideoBufferLayoutChanged()
{
	int w, h;
	SDL_GetWindowSize(this->sdl_window, &w, &h);
	this->ClientSizeChanged(w, h, true);
}

void VideoDriver_SDL_Base::ClientSizeChanged(int w, int h, bool force)
{
	/* Allocate backing store of the new size. */
	if (this->AllocateBackingStore(w, h, force)) {
		CopyPalette(this->local_palette, true);

		BlitterFactory::GetCurrentBlitter()->PostResize();

		GameSizeChanged();
	}
}

bool VideoDriver_SDL_Base::CreateMainWindow(uint w, uint h, uint flags)
{
	if (this->sdl_window != nullptr) return true;

	flags |= SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;

	if (_fullscreen) {
		flags |= SDL_WINDOW_FULLSCREEN;
	}

	int x = SDL_WINDOWPOS_UNDEFINED, y = SDL_WINDOWPOS_UNDEFINED;
	SDL_Rect r;
	if (SDL_GetDisplayBounds(this->startup_display, &r) == 0) {
		x = r.x + std::max(0, r.w - static_cast<int>(w)) / 2;
		y = r.y + std::max(0, r.h - static_cast<int>(h)) / 4; // decent desktops have taskbars at the bottom
	}

	std::string caption = VideoDriver::GetCaption();
	this->sdl_window = SDL_CreateWindow(
		caption.c_str(),
		x, y,
		w, h,
		flags);

	if (this->sdl_window == nullptr) {
		Debug(Facility::Driver, Severity::Critical, "SDL2: Couldn't allocate a window to draw on: {}", SDL_GetError());
		return false;
	}

	std::string icon_path = FioFindFullPath(Subdirectory::Baseset, "openttd.32.bmp");
	if (!icon_path.empty()) {
		/* Give the application an icon */
		SDL_Surface *icon = SDL_LoadBMP(icon_path.c_str());
		if (icon != nullptr) {
			/* Get the colourkey, which will be magenta */
			uint32_t rgbmap = SDL_MapRGB(icon->format, 255, 0, 255);

			SDL_SetColorKey(icon, SDL_TRUE, rgbmap);
			SDL_SetWindowIcon(this->sdl_window, icon);
			SDL_FreeSurface(icon);
		}
	}

	return true;
}

/**
 * Create/update the surfaces to draw OpenTTD on.
 * @param w The new width.
 * @param h The new height.
 * @param resize Whether this is a resize, or we are creating the window from scratch
 * @return \c true iff the surfaces could be created.
 */
bool VideoDriver_SDL_Base::CreateMainSurface(uint w, uint h, bool resize)
{
	GetAvailableVideoMode(&w, &h);
	Debug(Facility::Driver, Severity::Error, "SDL2: using mode {}x{}", w, h);

	if (!this->CreateMainWindow(w, h)) return false;
	if (resize) SDL_SetWindowSize(this->sdl_window, w, h);
	this->ClientSizeChanged(w, h, true);

	/* When in full screen, we will always have the mouse cursor
	 * within the window, even though SDL does not give us the
	 * appropriate event to know this. */
	if (_fullscreen) _cursor.in_window = true;

	return true;
}

void VideoDriver_SDL_Base::ClaimMousePointer()
{
	/* Emscripten never claims the pointer, so we do not need to change the cursor visibility. */
#ifndef __EMSCRIPTEN__
	SDL_ShowCursor(0);
#endif
}

/**
 * This is called to indicate that an edit box has gained focus, text input mode should be enabled.
 */
void VideoDriver_SDL_Base::EditBoxGainedFocus()
{
	if (!this->edit_box_focused) {
		SDL_StartTextInput();
		this->edit_box_focused = true;
	}
}

/**
 * This is called to indicate that an edit box has lost focus, text input mode should be disabled.
 */
void VideoDriver_SDL_Base::EditBoxLostFocus()
{
	if (this->edit_box_focused) {
		SDL_StopTextInput();
		this->edit_box_focused = false;
	}
}

std::vector<int> VideoDriver_SDL_Base::GetListOfMonitorRefreshRates()
{
	std::vector<int> rates = {};
	for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) {
		SDL_DisplayMode mode = {};
		if (SDL_GetDisplayMode(i, 0, &mode) != 0) continue;
		if (mode.refresh_rate != 0) rates.push_back(mode.refresh_rate);
	}
	return rates;
}


/** Mapping from keycodes in the SDL world to OpenTTD's world. */
struct SDLVkMapping {
	const SDL_Keycode vk_from; ///< The first of the SDL keycodes of this mapping.
	const WindowKeyCodes map_to; ///< The first of the OpenTTD keycodes of this mapping.
	const uint8_t count; ///< The number of keycodes that are to be mapped.
	const bool unprintable; ///< Whether this keycode is unprintable.

	/**
	 * Create a mapping for several consecutive keycodes.
	 * @param vk_first The first SDL keycode.
	 * @param vk_last The last SDL keycode (inclusive).
	 * @param map_first The first OpenTTD keycode.
	 * @param map_last The last OpenTTD keycode (inclusive).
	 * @param unprintable Whether this keycode is unprintable.
	 */
	constexpr SDLVkMapping(SDL_Keycode vk_first, SDL_Keycode vk_last, WindowKeyCodes map_first, [[maybe_unused]] WindowKeyCodes map_last, bool unprintable)
		: vk_from(vk_first), map_to(map_first), count(vk_last - vk_first + 1), unprintable(unprintable)
	{
		assert(vk_first < vk_last);
		assert(map_first < map_last);
		assert(this->count == static_cast<uint>(map_last - map_first + 1));
	}

	/**
	 * Create a mapping for a single keycode.
	 * @param vk_from The SDL keycode.
	 * @param map_to The OpenTTD keycode.
	 * @param unprintable Whether this keycode is unprintable.
	 */
	constexpr SDLVkMapping(SDL_Keycode vk_from, WindowKeyCodes map_to, bool unprintable)
		: vk_from(vk_from), map_to(map_to), count(1), unprintable(unprintable) {}
};

/** Mapping of SDL keycodes to WKC. */
static constexpr SDLVkMapping _vk_mapping[] = {
	/* All unprintable characters */
	{ SDLK_PAGEUP, WKC_PAGEUP, true },
	{ SDLK_PAGEDOWN, WKC_PAGEDOWN, true },
	{ SDLK_UP, WKC_UP, true },
	{ SDLK_DOWN, WKC_DOWN, true },
	{ SDLK_LEFT, WKC_LEFT, true },
	{ SDLK_RIGHT, WKC_RIGHT, true },

	{ SDLK_HOME, WKC_HOME, true },
	{ SDLK_END, WKC_END, true },

	{ SDLK_INSERT, WKC_INSERT, true },
	{ SDLK_DELETE, WKC_DELETE, true },

	{ SDLK_ESCAPE, WKC_ESC, true },
	{ SDLK_PAUSE, WKC_PAUSE, true },
	{ SDLK_BACKSPACE, WKC_BACKSPACE, true },

	{ SDLK_F1, SDLK_F12, WKC_F1, WKC_F12, true },

	/* Map letters & digits */
	{ SDLK_a, SDLK_z, WindowKeyCodes{'A'}, WindowKeyCodes{'Z'}, false },
	{ SDLK_0, SDLK_9, WindowKeyCodes{'0'}, WindowKeyCodes{'9'}, false },

	{ SDLK_SPACE, WKC_SPACE, false },
	{ SDLK_RETURN, WKC_RETURN, false },
	{ SDLK_TAB, WKC_TAB, false },

	/* Numeric part. */
	{ SDLK_KP_1, SDLK_KP_9, WindowKeyCodes{'1'}, WindowKeyCodes{'9'}, false },
	{ SDLK_KP_0, WindowKeyCodes{'0'}, false },
	{ SDLK_KP_DIVIDE, WKC_NUM_DIV, false },
	{ SDLK_KP_MULTIPLY, WKC_NUM_MUL, false },
	{ SDLK_KP_MINUS, WKC_NUM_MINUS, false },
	{ SDLK_KP_PLUS, WKC_NUM_PLUS, false },
	{ SDLK_KP_ENTER, WKC_NUM_ENTER, false },
	{ SDLK_KP_PERIOD, WKC_NUM_DECIMAL, false },

	/* Other non-letter keys */
	{ SDLK_SLASH, WKC_SLASH, false },
	{ SDLK_SEMICOLON, WKC_SEMICOLON, false },
	{ SDLK_EQUALS, WKC_EQUALS, false },
	{ SDLK_LEFTBRACKET, WKC_L_BRACKET, false },
	{ SDLK_BACKSLASH, WKC_BACKSLASH, false },
	{ SDLK_RIGHTBRACKET, WKC_R_BRACKET, false },

	{ SDLK_QUOTE, WKC_SINGLEQUOTE, false },
	{ SDLK_COMMA, WKC_COMMA, false },
	{ SDLK_MINUS, WKC_MINUS, false },
	{ SDLK_PERIOD, WKC_PERIOD, false },
};

/**
 * Convert the SDL key into our WindowKeyCode and the printable character (if any).
 * @param sym The keyboard event.
 * @param[out] character The pressed character (or \c '\0' when no character was pressed).
 * @return The pressed key.
 */
static uint ConvertSdlKeyIntoMy(SDL_Keysym *sym, char32_t *character)
{
	uint key = 0;
	bool unprintable = false;

	for (const auto &map : _vk_mapping) {
		if (IsInsideBS(sym->sym, map.vk_from, map.count)) {
			key = sym->sym - map.vk_from + map.map_to;
			unprintable = map.unprintable;
			break;
		}
	}

	/* check scancode for BACKQUOTE key, because we want the key left of "1", not anything else (on non-US keyboards) */
	if (sym->scancode == SDL_SCANCODE_GRAVE) key = WKC_BACKQUOTE;

	/* META are the command keys on mac */
	if (sym->mod & KMOD_GUI)   key |= WKC_META;
	if (sym->mod & KMOD_SHIFT) key |= WKC_SHIFT;
	if (sym->mod & KMOD_CTRL)  key |= WKC_CTRL;
	if (sym->mod & KMOD_ALT)   key |= WKC_ALT;

	/* The mod keys have no character. Prevent '?' */
	if (sym->mod & KMOD_GUI ||
		sym->mod & KMOD_CTRL ||
		sym->mod & KMOD_ALT ||
		unprintable) {
		*character = '\0';
	} else {
		*character = sym->sym;
	}

	return key;
}

/**
 * Convert a SDL_Keycode into our own key codes.
 * @param kc The SDL key code.
 * @return OpenTTD's internal key code.
 */
static uint ConvertSdlKeycodeIntoMy(SDL_Keycode kc)
{
	uint key = 0;

	for (const auto &map : _vk_mapping) {
		if (IsInsideBS(kc, map.vk_from, map.count)) {
			key = kc - map.vk_from + map.map_to;
			break;
		}
	}

	/* check scancode for BACKQUOTE key, because we want the key left
	 * of "1", not anything else (on non-US keyboards) */
	SDL_Scancode sc = SDL_GetScancodeFromKey(kc);
	if (sc == SDL_SCANCODE_GRAVE) key = WKC_BACKQUOTE;

	return key;
}

/** Fingers on the screen: one acts as the mouse, two move the view. */
static struct {
	struct Finger {
		SDL_FingerID id;
		TouchPoint pos;
	};
	std::array<Finger, 2> fingers{};
	int count = 0; ///< Number of fingers on the screen that are tracked.
	bool gesture = false; ///< Two fingers have touched since the screen was last free, so nothing clicks until it is free again.
	bool pending = false; ///< One finger touched and it is too early to tell whether a second one is coming.
	bool pressed = false; ///< One finger acts as the pressed left mouse button.
	TouchPoint start{}; ///< Where the pending finger touched.
	std::chrono::steady_clock::time_point since{}; ///< When the pending finger touched.
} _touch;

/** Moving this many pixels, or holding this long, makes a single finger a press rather than the start of a gesture. */
static constexpr float TOUCH_PRESS_DISTANCE = 12;
static constexpr auto TOUCH_PRESS_DELAY = std::chrono::milliseconds(100);

static void PressTouch()
{
	_touch.pending = false;
	/* The title bar of a panel moves the panel, rather than the window in the buffer. */
	if (StartXRPanelDrag(static_cast<int>(_touch.fingers[0].pos.x), static_cast<int>(_touch.fingers[0].pos.y))) return;
	if (ClickXRLabel(static_cast<int>(_touch.fingers[0].pos.x), static_cast<int>(_touch.fingers[0].pos.y))) return;
	_touch.pressed = true;
	_left_button_down = true;
	HandleMouseEvents();
}

static void ReleaseTouch()
{
	_touch.pressed = false;
	_left_button_down = false;
	_left_button_clicked = false;
	HandleMouseEvents();
}

static void MoveTouchCursor(TouchPoint pos)
{
	_cursor.in_window = true;
	const Point cursor = MapScreenToXRPanels(static_cast<int>(pos.x), static_cast<int>(pos.y));
	_cursor.UpdateCursorPosition(cursor.x, cursor.y);
	HandleMouseEvents();
}

static void HandleFingerEvent(const SDL_TouchFingerEvent &finger)
{
	/* Fingers can only be on the screen, whatever the video buffer has below it. */
	const TouchPoint pos{finger.x * _screen.width, finger.y * (_screen.height - GetXRAtlasExtraHeight())};
	auto tracked = std::find_if(_touch.fingers.begin(), _touch.fingers.begin() + _touch.count, [&](const auto &f) { return f.id == finger.fingerId; });
	const bool known = tracked != _touch.fingers.begin() + _touch.count;

	switch (finger.type) {
		case SDL_FINGERDOWN:
			if (_touch.count == static_cast<int>(_touch.fingers.size())) break;
			_touch.fingers[_touch.count++] = {finger.fingerId, pos};
			if (_touch.count == 1) {
				_touch.pending = true;
				_touch.start = pos;
				_touch.since = std::chrono::steady_clock::now();
				MoveTouchCursor(pos);
			} else {
				_touch.pending = false;
				_touch.gesture = true;
				if (_touch.pressed) ReleaseTouch();
			}
			break;

		case SDL_FINGERMOTION:
			if (!known) break;
			if (_touch.count == 2) {
				const auto other = _touch.fingers[tracked == _touch.fingers.begin() ? 1 : 0].pos;
				const auto step = GetTwoFingerStep(tracked->pos, other, pos, other);
				HandleTwoFingerGesture(step.pan_x, step.pan_y, step.scale, step.rotate);
				CountXRTestInput("two_finger_step", fmt::format("pan={:.1f},{:.1f} scale={:.3f} rotate={:.2f}", step.pan_x, step.pan_y, step.scale, step.rotate));
			} else if (!_touch.gesture) {
				if (_touch.pending && std::hypot(pos.x - _touch.start.x, pos.y - _touch.start.y) >= TOUCH_PRESS_DISTANCE) PressTouch();
				if (IsXRPanelDragging()) {
					MoveXRPanelDrag(static_cast<int>(pos.x), static_cast<int>(pos.y));
				} else if (_touch.pressed) {
					MoveTouchCursor(pos);
				}
			}
			tracked->pos = pos;
			break;

		case SDL_FINGERUP:
			if (!known) break;
			/* A short tap never got to be a press, but is a click all the same. */
			if (_touch.pending) PressTouch();
			if (_touch.pressed) ReleaseTouch();
			EndXRPanelDrag();
			*tracked = _touch.fingers[_touch.count - 1];
			if (--_touch.count == 0 && _touch.gesture) {
				_touch.gesture = false;
				EndTwoFingerGesture();
			}
			break;

		default: break;
	}
}

/**
 * Feed a two finger gesture through the event queue, for testing without two real fingers.
 * @param pan_x Pixels to move to the right.
 * @param pan_y Pixels to move down.
 * @param scale How many times further apart the fingers end up.
 * @param rotate Degrees to turn clockwise.
 */
void PushTwoFingerTestGesture(float pan_x, float pan_y, float scale, float rotate)
{
	constexpr int STEPS = 10;
	auto send = [](Uint32 type, SDL_FingerID id, float x, float y) {
		SDL_Event ev{};
		ev.tfinger.type = type;
		ev.tfinger.fingerId = id;
		ev.tfinger.x = x / _screen.width;
		ev.tfinger.y = y / (_screen.height - GetXRAtlasExtraHeight());
		SDL_PushEvent(&ev);
	};
	auto at = [&](int step, float sign) {
		const float t = static_cast<float>(step) / STEPS;
		const float height = static_cast<float>(_screen.height - GetXRAtlasExtraHeight());
		const float angle = rotate * t * 3.14159265359f / 180, radius = height * 0.2f * std::pow(scale, t) * sign;
		return TouchPoint{_screen.width * 0.5f + pan_x * t + radius * std::cos(angle), height * 0.5f + pan_y * t + radius * std::sin(angle)};
	};
	send(SDL_FINGERDOWN, 1, at(0, 1).x, at(0, 1).y);
	send(SDL_FINGERDOWN, 2, at(0, -1).x, at(0, -1).y);
	for (int i = 1; i <= STEPS; i++) {
		send(SDL_FINGERMOTION, 1, at(i, 1).x, at(i, 1).y);
		send(SDL_FINGERMOTION, 2, at(i, -1).x, at(i, -1).y);
	}
	send(SDL_FINGERUP, 1, at(STEPS, 1).x, at(STEPS, 1).y);
	send(SDL_FINGERUP, 2, at(STEPS, -1).x, at(STEPS, -1).y);
}

/** Note an event in the test log: input is counted, the life of the window and the app is written as it happens. */
static void LogTestEvent(const SDL_Event &ev)
{
	auto touch_type = [](SDL_TouchID id) {
		switch (SDL_GetTouchDeviceType(id)) {
			case SDL_TOUCH_DEVICE_DIRECT: return "direct";
			case SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE: return "indirect_absolute";
			case SDL_TOUCH_DEVICE_INDIRECT_RELATIVE: return "indirect_relative";
			default: return "unknown";
		}
	};
	auto mouse = [](uint32_t which) { return which == SDL_TOUCH_MOUSEID ? "touch" : "mouse"; };
	switch (ev.type) {
		case SDL_FINGERDOWN: case SDL_FINGERUP: case SDL_FINGERMOTION:
			CountXRTestInput(ev.type == SDL_FINGERDOWN ? "finger_down" : ev.type == SDL_FINGERUP ? "finger_up" : "finger_motion",
				fmt::format("device={} ({}) finger={} x={:.3f} y={:.3f} pressure={:.2f}", ev.tfinger.touchId, touch_type(ev.tfinger.touchId),
				ev.tfinger.fingerId, ev.tfinger.x, ev.tfinger.y, ev.tfinger.pressure));
			break;
		case SDL_MOUSEMOTION:
			CountXRTestInput(fmt::format("{}_motion", mouse(ev.motion.which)), fmt::format("which={} x={} y={} buttons={}", ev.motion.which, ev.motion.x, ev.motion.y, ev.motion.state));
			break;
		case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
			CountXRTestInput(fmt::format("{}_{}", mouse(ev.button.which), ev.type == SDL_MOUSEBUTTONDOWN ? "down" : "up"),
				fmt::format("which={} button={} clicks={} x={} y={}", ev.button.which, ev.button.button, ev.button.clicks, ev.button.x, ev.button.y));
			break;
		case SDL_MOUSEWHEEL:
			CountXRTestInput("wheel", fmt::format("which={} x={} y={}", ev.wheel.which, ev.wheel.x, ev.wheel.y));
			break;
		case SDL_KEYDOWN: case SDL_KEYUP:
			CountXRTestInput(ev.type == SDL_KEYDOWN ? "key_down" : "key_up", fmt::format("scancode={} ({}) sym={} mod={}",
				static_cast<int>(ev.key.keysym.scancode), SDL_GetScancodeName(ev.key.keysym.scancode), ev.key.keysym.sym, ev.key.keysym.mod));
			break;
		case SDL_TEXTINPUT:
			CountXRTestInput("text", "");
			break;
		case SDL_WINDOWEVENT: {
			static constexpr std::array<const char *, 17> names{"none", "shown", "hidden", "exposed", "moved", "resized", "size_changed",
				"minimized", "maximized", "restored", "enter", "leave", "focus_gained", "focus_lost", "close", "take_focus", "hit_test"};
			const char *name = ev.window.event < names.size() ? names[ev.window.event] : "other";
			if (ev.window.event == SDL_WINDOWEVENT_EXPOSED || ev.window.event == SDL_WINDOWEVENT_ENTER || ev.window.event == SDL_WINDOWEVENT_LEAVE) {
				CountXRTestInput(fmt::format("window_{}", name), "");
			} else {
				XRTestLog("window: {} {} {}", name, ev.window.data1, ev.window.data2);
			}
			break;
		}
		case SDL_DISPLAYEVENT: XRTestLog("display: event={} data={}", ev.display.event, ev.display.data1); break;
		case SDL_APP_TERMINATING: XRTestLog("app: terminating"); break;
		case SDL_APP_LOWMEMORY: XRTestLog("app: low memory"); break;
		case SDL_APP_WILLENTERBACKGROUND: XRTestLog("app: entering background"); break;
		case SDL_APP_DIDENTERBACKGROUND: XRTestLog("app: in background"); break;
		case SDL_APP_WILLENTERFOREGROUND: XRTestLog("app: entering foreground"); break;
		case SDL_APP_DIDENTERFOREGROUND: XRTestLog("app: in foreground"); break;
		case SDL_RENDER_TARGETS_RESET: XRTestLog("render targets reset"); break;
		case SDL_RENDER_DEVICE_RESET: XRTestLog("render device reset"); break;
		case SDL_JOYDEVICEADDED: case SDL_CONTROLLERDEVICEADDED: XRTestLog("controller added: event={:#x} id={}", ev.type, ev.cdevice.which); break;
		case SDL_QUIT: XRTestLog("quit requested"); break;
		default: CountXRTestInput(fmt::format("sdl_{:#x}", ev.type), ""); break;
	}
}

bool VideoDriver_SDL_Base::PollEvent()
{
	SDL_Event ev;

	if (_touch.pending && std::chrono::steady_clock::now() - _touch.since >= TOUCH_PRESS_DELAY) PressTouch();

	if (!SDL_PollEvent(&ev)) return false;
	if (IsXRTestLogOpen()) LogTestEvent(ev);

	switch (ev.type) {
		case SDL_FINGERDOWN:
		case SDL_FINGERMOTION:
		case SDL_FINGERUP:
			HandleFingerEvent(ev.tfinger);
			break;

		case SDL_MOUSEMOTION: {
			int32_t x = ev.motion.x;
			int32_t y = ev.motion.y;

			if (_cursor.fix_at) {
				/* Get all queued mouse events now in case we have to warp the cursor. In the
				 * end, we only care about the current mouse position and not bygone events. */
				while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_MOUSEMOTION, SDL_MOUSEMOTION)) {
					x = ev.motion.x;
					y = ev.motion.y;
				}
			}

			if (IsXRPanelDragging()) {
				MoveXRPanelDrag(x, y);
				break;
			}
			const Point cursor = MapScreenToXRPanels(x, y);
			if (_cursor.UpdateCursorPosition(cursor.x, cursor.y) && !IsXRPanelsActive()) {
				SDL_WarpMouseInWindow(this->sdl_window, _cursor.pos.x, _cursor.pos.y);
			}
			HandleMouseEvents();
			break;
		}

		case SDL_MOUSEWHEEL: {
			if (ev.wheel.y > 0) {
				_cursor.wheel--;
			} else if (ev.wheel.y < 0) {
				_cursor.wheel++;
			}

			/* Handle 2D scrolling. */
			const float SCROLL_BUILTIN_MULTIPLIER = 14.0f;
#if SDL_VERSION_ATLEAST(2, 18, 0)
			_cursor.v_wheel -= ev.wheel.preciseY * SCROLL_BUILTIN_MULTIPLIER * _settings_client.gui.scrollwheel_multiplier;
			_cursor.h_wheel += ev.wheel.preciseX * SCROLL_BUILTIN_MULTIPLIER * _settings_client.gui.scrollwheel_multiplier;
#else
			_cursor.v_wheel -= static_cast<float>(ev.wheel.y * SCROLL_BUILTIN_MULTIPLIER * _settings_client.gui.scrollwheel_multiplier);
			_cursor.h_wheel += static_cast<float>(ev.wheel.x * SCROLL_BUILTIN_MULTIPLIER * _settings_client.gui.scrollwheel_multiplier);
#endif
			_cursor.wheel_moved = true;
			break;
		}

		case SDL_MOUSEBUTTONDOWN:
			if (_rightclick_emulate && SDL_GetModState() & KMOD_CTRL) {
				ev.button.button = SDL_BUTTON_RIGHT;
			}

			switch (ev.button.button) {
				case SDL_BUTTON_LEFT:
					if (StartXRPanelDrag(ev.button.x, ev.button.y) || ClickXRLabel(ev.button.x, ev.button.y)) break;
					_left_button_down = true;
					break;

				case SDL_BUTTON_RIGHT:
					_right_button_down = true;
					_right_button_clicked = true;
					break;

				case SDL_BUTTON_MIDDLE:
					_middle_button_down = true;
					_middle_button_clicked = true;
					break;

				default: break;
			}
			HandleMouseEvents();
			break;

		case SDL_MOUSEBUTTONUP:
			if (_rightclick_emulate) {
				_right_button_down = false;
				_left_button_down = false;
				_left_button_clicked = false;
			} else if (ev.button.button == SDL_BUTTON_LEFT) {
				EndXRPanelDrag();
				_left_button_down = false;
				_left_button_clicked = false;
			} else if (ev.button.button == SDL_BUTTON_RIGHT) {
				_right_button_down = false;
			} else if (ev.button.button == SDL_BUTTON_MIDDLE) {
				_middle_button_down = false;
				_middle_button_clicked = false;
			}
			HandleMouseEvents();
			break;

		case SDL_QUIT:
			HandleExitGameRequest();
			break;

		case SDL_KEYDOWN: // Toggle full-screen on ALT + ENTER/F
			if ((ev.key.keysym.mod & (KMOD_ALT | KMOD_GUI)) &&
					(ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_f)) {
				if (ev.key.repeat == 0) ToggleFullScreen(!_fullscreen);
			} else {
				char32_t character;

				uint keycode = ConvertSdlKeyIntoMy(&ev.key.keysym, &character);
				/* Only handle non-text keys here. Text is handled in
				 * SDL_TEXTINPUT below. */
				if (!this->edit_box_focused ||
					keycode == WKC_DELETE ||
					keycode == WKC_NUM_ENTER ||
					keycode == WKC_LEFT ||
					keycode == WKC_RIGHT ||
					keycode == WKC_UP ||
					keycode == WKC_DOWN ||
					keycode == WKC_HOME ||
					keycode == WKC_END ||
					keycode & WKC_META ||
					keycode & WKC_CTRL ||
					keycode & WKC_ALT ||
					(keycode >= WKC_F1 && keycode <= WKC_F12) ||
					!IsValidChar(character, CS_ALPHANUMERAL)) {
					HandleKeypress(keycode, character);
				}
			}
			break;

		case SDL_TEXTINPUT: {
			if (!this->edit_box_focused) break;
			SDL_Keycode kc = SDL_GetKeyFromName(ev.text.text);
			uint keycode = ConvertSdlKeycodeIntoMy(kc);

			if (keycode == WKC_BACKQUOTE && FocusedWindowIsConsole()) {
				auto [len, c] = DecodeUtf8(ev.text.text);
				if (len > 0) HandleKeypress(keycode, c);
			} else {
				HandleTextInput(ev.text.text);
			}
			break;
		}
		case SDL_WINDOWEVENT: {
			if (ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
				/* Force a redraw of the entire screen. */
				this->MakeDirty(0, 0, _screen.width, _screen.height);
			} else if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
				int w = std::max(ev.window.data1, 64);
				int h = std::max(ev.window.data2, 64);
				CreateMainSurface(w, h, w != ev.window.data1 || h != ev.window.data2);
			} else if (ev.window.event == SDL_WINDOWEVENT_ENTER) {
				/* mouse entered the window, enable cursor */
				_cursor.in_window = true;
				/* Ensure pointer lock will not occur. */
				SDL_SetRelativeMouseMode(SDL_FALSE);
			} else if (ev.window.event == SDL_WINDOWEVENT_LEAVE) {
				/* mouse left the window, undraw cursor */
				UndrawMouseCursor();
				_cursor.in_window = false;
			}
			break;
		}
	}

	return true;
}

/**
 * Initialize the SDL backend if needed.
 * @return An optional with the error message upon failure.
 */
static std::optional<std::string_view> InitializeSDL()
{
	/* Check if the video-driver is already initialized. */
	if (SDL_WasInit(SDL_INIT_VIDEO) != 0) return std::nullopt;

#ifdef SDL_HINT_APP_NAME
	SDL_SetHint(SDL_HINT_APP_NAME, "OpenTTD");
#endif

	/* Touches are handled below rather than arriving as a mouse. */
#ifndef __EMSCRIPTEN__
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif

	if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return SDL_GetError();
	return std::nullopt;
}

/**
 * Initialize the driver.
 * @return An optional with the error message upon failure.
 */
std::optional<std::string_view> VideoDriver_SDL_Base::Initialize()
{
	this->UpdateAutoResolution();

	auto error = InitializeSDL();
	if (error) return error;

	FindResolutions();
	Debug(Facility::Driver, Severity::Warning, "Resolution for display: {}x{}", _cur_resolution.width, _cur_resolution.height);

	return std::nullopt;
}

std::optional<std::string_view> VideoDriver_SDL_Base::Start(const StringList &param)
{
	if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() == 0) return "Only real blitters supported";

	auto error = this->Initialize();
	if (error) return error;

#ifdef SDL_HINT_MOUSE_AUTO_CAPTURE
	if (GetDriverParamBool(param, "no_mouse_capture")) {
		/* By default SDL captures the mouse, while a button is pressed.
		 * This is annoying during debugging, when OpenTTD is suspended while the button was pressed.
		 */
		if (!SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0")) return SDL_GetError();
	}
#endif

	this->startup_display = FindStartupDisplay(GetDriverParamInt(param, "display", -1));

	if (!CreateMainSurface(_cur_resolution.width, _cur_resolution.height, false)) {
		return SDL_GetError();
	}

	const char *dname = SDL_GetCurrentVideoDriver();
	Debug(Facility::Driver, Severity::Error, "SDL2: using driver '{}'", dname);

	this->driver_info = this->GetName();
	this->driver_info += " (";
	this->driver_info += dname;
	this->driver_info += ")";

	MarkWholeScreenDirty();

	SDL_StopTextInput();
	this->edit_box_focused = false;

#ifdef __EMSCRIPTEN__
	this->is_game_threaded = false;
#else
	this->is_game_threaded = !GetDriverParamBool(param, "no_threads") && !GetDriverParamBool(param, "no_thread");
#endif

	return std::nullopt;
}

void VideoDriver_SDL_Base::Stop()
{
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
	if (SDL_WasInit(SDL_INIT_EVERYTHING) == 0) {
		SDL_Quit(); // If there's nothing left, quit SDL
	}
}

void VideoDriver_SDL_Base::InputLoop()
{
	uint32_t mod = SDL_GetModState();
	const Uint8 *keys = SDL_GetKeyboardState(nullptr);

	bool old_ctrl_pressed = _ctrl_pressed;

	_ctrl_pressed  = !!(mod & KMOD_CTRL);
	_shift_pressed = !!(mod & KMOD_SHIFT);

	/* Speedup when pressing tab, except when using ALT+TAB
	 * to switch to another application. */
	this->fast_forward_key_pressed = keys[SDL_SCANCODE_TAB] && (mod & KMOD_ALT) == 0;

	/* Determine which directional keys are down. */
	_dirkeys.Set(DirectionKey::Left, keys[SDL_SCANCODE_LEFT]);
	_dirkeys.Set(DirectionKey::Up, keys[SDL_SCANCODE_UP]);
	_dirkeys.Set(DirectionKey::Right, keys[SDL_SCANCODE_RIGHT]);
	_dirkeys.Set(DirectionKey::Down, keys[SDL_SCANCODE_DOWN]);

	if (old_ctrl_pressed != _ctrl_pressed) HandleCtrlChanged();
}

/** Run a single tick of the game/main loop. */
void VideoDriver_SDL_Base::LoopOnce()
{
	if (_exit_game) {
#ifdef __EMSCRIPTEN__
		/* Emscripten is event-driven, and as such the main loop is inside
		 * the browser. So if _exit_game goes true, the main loop ends (the
		 * cancel call), but we still have to call the cleanup that is
		 * normally done at the end of the main loop for non-Emscripten.
		 * After that, Emscripten just halts, and the HTML shows a nice
		 * "bye, see you next time" message. */
		extern void PostMainLoop();
		PostMainLoop();

		emscripten_cancel_main_loop();
		emscripten_exit_pointerlock();
		/* In effect, the game ends here. As emscripten_set_main_loop() caused
		 * the stack to be unwound, the code after MainLoop() in
		 * openttd_main() is never executed. */
		if (_game_mode == GameMode::Bootstrap) {
			EM_ASM(if (window["openttd_bootstrap_reload"]) openttd_bootstrap_reload());
		} else {
			EM_ASM(if (window["openttd_exit"]) openttd_exit());
		}
#endif
		return;
	}

	this->Tick();

/* Emscripten is running an event-based mainloop; there is already some
 * downtime between each iteration, so no need to sleep. */
#ifndef __EMSCRIPTEN__
	this->SleepTillNextTick();
#endif
}

void VideoDriver_SDL_Base::MainLoop()
{
#ifdef __EMSCRIPTEN__
	/* Run the main loop event-driven, based on RequestAnimationFrame. */
	emscripten_set_main_loop_arg(&this->EmscriptenLoop, this, 0, 1);
#else
	this->StartGameThread();

	while (!_exit_game) {
		LoopOnce();
	}

	this->StopGameThread();
#endif
}

bool VideoDriver_SDL_Base::ChangeResolution(int w, int h)
{
	return CreateMainSurface(w, h, true);
}

bool VideoDriver_SDL_Base::ToggleFullscreen(bool fullscreen)
{
	/* Remember current window size */
	int w, h;
	SDL_GetWindowSize(this->sdl_window, &w, &h);

	if (fullscreen) {
		/* Find fullscreen window size */
		SDL_DisplayMode dm;
		if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(this->sdl_window), &dm) < 0) {
			Debug(Facility::Driver, Severity::Critical, "SDL_GetCurrentDisplayMode() failed: {}", SDL_GetError());
		} else {
			SDL_SetWindowSize(this->sdl_window, dm.w, dm.h);
		}
	}

	Debug(Facility::Driver, Severity::Error, "SDL2: Setting {}", fullscreen ? "fullscreen" : "windowed");
	int ret = SDL_SetWindowFullscreen(this->sdl_window, fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
	if (ret == 0) {
		/* Switching resolution succeeded, set fullscreen value of window. */
		_fullscreen = fullscreen;
		if (!fullscreen) SDL_SetWindowSize(this->sdl_window, w, h);
	} else {
		Debug(Facility::Driver, Severity::Critical, "SDL_SetWindowFullscreen() failed: {}", SDL_GetError());
	}

	InvalidateWindowClassesData(WindowClass::GameOptions, 3);
	return ret == 0;
}

bool VideoDriver_SDL_Base::AfterBlitterChange()
{
	assert(BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 0);
	int w, h;
	SDL_GetWindowSize(this->sdl_window, &w, &h);
	return CreateMainSurface(w, h, false);
}

Dimension VideoDriver_SDL_Base::GetScreenSize() const
{
	SDL_DisplayMode mode;
	if (SDL_GetCurrentDisplayMode(this->startup_display, &mode) != 0) return VideoDriver::GetScreenSize();

	return { static_cast<uint>(mode.w), static_cast<uint>(mode.h) };
}

bool VideoDriver_SDL_Base::LockVideoBuffer()
{
	if (this->buffer_locked) return false;
	this->buffer_locked = true;

	_screen.dst_ptr = this->GetVideoPointer();
	assert(_screen.dst_ptr != nullptr);

	return true;
}

void VideoDriver_SDL_Base::UnlockVideoBuffer()
{
	if (_screen.dst_ptr != nullptr) {
		/* Hand video buffer back to the drawing backend. */
		this->ReleaseVideoPointer();
		_screen.dst_ptr = nullptr;
	}

	this->buffer_locked = false;
}

void VideoDriver_SDL_Base::SetScreensaverInhibited(bool inhibited)
{
	if (inhibited) {
		SDL_DisableScreenSaver();
	} else {
		SDL_EnableScreenSaver();
	}
}
