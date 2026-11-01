/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file touch_gesture.h How two moving fingers translate to panning, zooming and rotating. */

#ifndef VIDEO_TOUCH_GESTURE_H
#define VIDEO_TOUCH_GESTURE_H

#include <cmath>

/** Position of a finger on the screen, in pixels. */
struct TouchPoint {
	float x, y;
};

/** What two fingers did between two moments. */
struct TwoFingerStep {
	float pan_x; ///< Pixels the point between the fingers moved to the right.
	float pan_y; ///< Pixels the point between the fingers moved down.
	float scale; ///< How many times further apart the fingers are.
	float rotate; ///< Degrees the line between the fingers turned, clockwise on the screen.
};

/**
 * Work out a step of a two finger gesture.
 * @param a0 First finger before.
 * @param b0 Second finger before.
 * @param a1 First finger after.
 * @param b1 Second finger after.
 * @return The pan, zoom and rotation between the two moments.
 */
inline TwoFingerStep GetTwoFingerStep(TouchPoint a0, TouchPoint b0, TouchPoint a1, TouchPoint b1)
{
	constexpr float PI = 3.14159265359f;
	/* With the fingers almost together their line has no meaningful length or direction. */
	constexpr float MIN_SPAN = 8;

	const float dx0 = b0.x - a0.x, dy0 = b0.y - a0.y, dx1 = b1.x - a1.x, dy1 = b1.y - a1.y;
	const float span0 = std::hypot(dx0, dy0), span1 = std::hypot(dx1, dy1);

	TwoFingerStep step{(a1.x + b1.x - a0.x - b0.x) / 2, (a1.y + b1.y - a0.y - b0.y) / 2, 1, 0};
	if (span0 < MIN_SPAN || span1 < MIN_SPAN) return step;

	step.scale = span1 / span0;
	float turn = std::atan2(dy1, dx1) - std::atan2(dy0, dx0);
	if (turn > PI) turn -= 2 * PI;
	if (turn < -PI) turn += 2 * PI;
	step.rotate = turn * 180 / PI;
	return step;
}

#endif /* VIDEO_TOUCH_GESTURE_H */
