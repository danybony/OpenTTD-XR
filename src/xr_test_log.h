/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file xr_test_log.h A log of a test session on a device, which a tester without debugging tools can send back. */

#ifndef XR_TEST_LOG_H
#define XR_TEST_LOG_H

#include "core/format.hpp"
#include <chrono>
#include <string>
#include <string_view>

/** Start appending to the test log at this path; until then the test log does nothing. */
void OpenXRTestLog(const std::string &path);
/** Whether there is a test log to write to. */
bool IsXRTestLogOpen();
/** Add a line to the test log, with the time in front of it. */
void WriteXRTestLog(std::string_view line);

/** Add a formatted line to the test log. */
template <typename... Args>
void XRTestLog(fmt::format_string<Args...> format, Args &&... args)
{
	if (IsXRTestLogOpen()) WriteXRTestLog(fmt::format(format, std::forward<Args>(args)...));
}

/**
 * Count an input event, describing the first few of each kind in full.
 * @param kind What sort of event it is.
 * @param detail What there is to know about this one.
 */
void CountXRTestInput(std::string_view kind, std::string_view detail);

/**
 * A frame has been drawn: keep its timing for the next summary.
 * @param paint_start When drawing the frame began.
 */
void XRTestLogFrame(std::chrono::steady_clock::time_point paint_start);
/** With the game state locked: write what changed, and now and then how the frames and the input went. */
void XRTestLogTick();

/** Write the current state and hand the test log to the system's share sheet. */
void ShareXRTestLog();
/** Whether the test log can be shared from this device. */
bool CanShareXRTestLog();

#endif /* XR_TEST_LOG_H */
