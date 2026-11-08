/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/**
 * @file xr_test_log.cpp A log of a test session on a device, which a tester without debugging tools can send back.
 *
 * On Android the activity starts the file with what it knows of the device, and the game adds what it sees: the
 * graphics driver, input events, how fast frames come, and the 3D settings as they change. Every line is also in
 * logcat. The 3D menu hands the file, with the previous session's and the latest crash report, to the share sheet.
 */

#include "stdafx.h"
#include "xr_test_log.h"
#include "gfx_func.h"
#include "settings_type.h"
#include "viewport_type.h"
#include "window_func.h"
#include "window_gui.h"
#include "xr_panels.h"
#include "video/proof3d.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <vector>

#ifdef __ANDROID__
#	include <android/log.h>
#	include <jni.h>
#	include <SDL.h>
#endif

#include "safeguards.h"

static constexpr long MAX_LOG_BYTES = 8 * 1024 * 1024; ///< Stop writing beyond this, so a long session cannot fill the device.
static constexpr auto SUMMARY_INTERVAL = std::chrono::seconds(10); ///< How often to write how the frames went.
static constexpr int DETAILED_EVENTS = 5; ///< Input events of each kind written in full before they are only counted.

static std::mutex _log_mutex;
static FILE *_log_file = nullptr;
static bool _log_full = false;

void OpenXRTestLog(const std::string &path)
{
	std::lock_guard lock(_log_mutex);
	if (_log_file != nullptr) return;
	_log_file = fopen(path.c_str(), "a");
}

bool IsXRTestLogOpen()
{
	return _log_file != nullptr && !_log_full;
}

void WriteXRTestLog(std::string_view line)
{
	using namespace std::chrono;
	const auto now = system_clock::now();
	const std::time_t seconds = system_clock::to_time_t(now);
	std::tm local{};
	localtime_r(&seconds, &local);
	const auto millis = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

	std::lock_guard lock(_log_mutex);
	if (_log_file == nullptr || _log_full) return;
	fmt::print(_log_file, "{:02}:{:02}:{:02}.{:03} {}\n", local.tm_hour, local.tm_min, local.tm_sec, millis, line);
	if (ftell(_log_file) > MAX_LOG_BYTES) {
		fmt::print(_log_file, "log full, nothing more is written\n");
		_log_full = true;
	}
	fflush(_log_file);
#ifdef __ANDROID__
	__android_log_print(ANDROID_LOG_INFO, "OpenTTD-XRTest", "%.*s", static_cast<int>(line.size()), line.data());
#endif
}

/** Input events since the last summary, by kind. */
static std::map<std::string, int, std::less<>> _input_counts;
/** All input events of each kind, to know which were written in full. */
static std::map<std::string, int, std::less<>> _input_seen;

void CountXRTestInput(std::string_view kind, std::string_view detail)
{
	if (!IsXRTestLogOpen()) return;
	auto seen = _input_seen.find(kind);
	if (seen == _input_seen.end()) seen = _input_seen.emplace(std::string{kind}, 0).first;
	if (seen->second++ < DETAILED_EVENTS) XRTestLog("input {} #{}: {}", kind, seen->second, detail);
	auto count = _input_counts.find(kind);
	if (count == _input_counts.end()) count = _input_counts.emplace(std::string{kind}, 0).first;
	count->second++;
}

/** What is on screen and how the 3D view is set up; a line is written whenever it changes. */
struct TestState {
	int width = 0, height = 0;
	bool tabletop = false;
	int table_size = 0;
	bool panels = false;
	int stereo = 0;
	int shadows = 0;
	bool retro = false;
	int gui_scale = 0;

	bool operator==(const TestState &) const = default;
};

static TestState CurrentTestState()
{
	TestState state;
	state.width = _screen.width;
	state.height = _screen.height - GetXRAtlasExtraHeight();
	state.gui_scale = _gui_scale;
#ifdef WITH_3D_PROOF
	const Window *main_window = FindWindowById(WindowClass::MainWindow, 0);
	state.tabletop = main_window != nullptr && main_window->viewport != nullptr && IsTerrain3DTabletop(*main_window->viewport);
	state.table_size = _settings_client.gui.renderer_3d_table_size;
	state.panels = IsXRPanelsActive();
	state.stereo = GetProof3DStereo();
	state.shadows = _settings_client.gui.renderer_3d_shadows;
	state.retro = _settings_client.gui.renderer_3d_retro;
#endif
	return state;
}

static void WriteTestState(const TestState &state)
{
	XRTestLog("state: screen={}x{} gui_scale={} tabletop={} table={} panels={} stereo={} shadows={} retro={}",
		state.width, state.height, state.gui_scale, state.tabletop, state.table_size, state.panels, state.stereo, state.shadows, state.retro);
}

/** Resident memory of the process in megabytes, or -1 when unknown. */
static long ResidentMegabytes()
{
	FILE *f = fopen("/proc/self/statm", "r");
	if (f == nullptr) return -1;
	long size = 0, resident = 0;
	const bool read = fscanf(f, "%ld %ld", &size, &resident) == 2;
	fclose(f);
	return read ? resident * 4096 / (1024 * 1024) : -1;
}

/** How busy an Adreno GPU is, in percent, or -1 where the system does not tell. */
static int GpuBusyPercent()
{
	FILE *f = fopen("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", "r");
	if (f == nullptr) return -1;
	int busy = -1;
	if (fscanf(f, "%d", &busy) != 1) busy = -1;
	fclose(f);
	return busy;
}

/** Frame timings since the last summary; only touched by the thread that draws, which also runs the video tick. */
static std::vector<float> _frame_intervals, _frame_draws;

void XRTestLogFrame(std::chrono::steady_clock::time_point paint_start)
{
	if (!IsXRTestLogOpen()) return;
	using Clock = std::chrono::steady_clock;
	static Clock::time_point last_frame{};
	const auto now = Clock::now();
	auto millis = [](Clock::duration d) { return std::chrono::duration<float, std::milli>(d).count(); };
	if (last_frame != Clock::time_point{}) _frame_intervals.push_back(millis(now - last_frame));
	_frame_draws.push_back(millis(now - paint_start));
	last_frame = now;
}

void XRTestLogTick()
{
	if (!IsXRTestLogOpen()) return;
	using Clock = std::chrono::steady_clock;
	static Clock::time_point last_summary = Clock::now();
	static TestState last_state{};
	static bool first = true;

	const TestState state = CurrentTestState();
	if (first || !(state == last_state)) {
		WriteTestState(state);
		last_state = state;
		first = false;
	}

	const auto now = Clock::now();
	if (now - last_summary < SUMMARY_INTERVAL) return;
	const float seconds = std::chrono::duration<float>(now - last_summary).count();
	last_summary = now;
	auto &intervals = _frame_intervals, &draws = _frame_draws;
	if (!intervals.empty() && !draws.empty()) {
		float total = 0, draw_total = 0;
		for (float v : intervals) total += v;
		for (float v : draws) draw_total += v;
		std::sort(intervals.begin(), intervals.end());
		const float p95 = intervals[std::min(intervals.size() - 1, static_cast<size_t>(intervals.size() * 0.95f))];
		XRTestLog("perf: fps={:.1f} frame_ms avg={:.1f} p95={:.1f} max={:.1f} draw_ms avg={:.1f} max={:.1f} rss_mb={} gpu_busy={}",
			intervals.size() / seconds, total / intervals.size(), p95, intervals.back(), draw_total / draws.size(),
			*std::max_element(draws.begin(), draws.end()), ResidentMegabytes(), GpuBusyPercent());
	} else {
		XRTestLog("perf: no frames in {:.0f} s", seconds);
	}
	intervals.clear();
	draws.clear();

	if (!_input_counts.empty()) {
		std::string counts;
		for (const auto &[kind, count] : _input_counts) fmt::format_to(std::back_inserter(counts), " {}={}", kind, count);
		XRTestLog("input counts:{}", counts);
		_input_counts.clear();
	}
}

#ifdef __ANDROID__
/** Run a method of the activity that takes nothing and returns nothing. */
static bool CallActivity(const char *method)
{
	JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
	jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
	if (env == nullptr || activity == nullptr) return false;
	jclass type = env->GetObjectClass(activity);
	jmethodID id = env->GetMethodID(type, method, "()V");
	if (id != nullptr) env->CallVoidMethod(activity, id);
	if (env->ExceptionCheck()) {
		env->ExceptionDescribe();
		env->ExceptionClear();
		id = nullptr;
	}
	env->DeleteLocalRef(type);
	env->DeleteLocalRef(activity);
	return id != nullptr;
}
#endif

bool CanShareXRTestLog()
{
#ifdef __ANDROID__
	return _log_file != nullptr;
#else
	return false;
#endif
}

void ShareXRTestLog()
{
	XRTestLog("share requested");
	WriteTestState(CurrentTestState());
#ifdef __ANDROID__
	if (!CallActivity("shareTestLog")) XRTestLog("share failed: the activity cannot be reached");
#endif
}
