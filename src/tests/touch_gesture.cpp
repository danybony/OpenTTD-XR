/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file touch_gesture.cpp Tests for how two moving fingers translate to panning, zooming and rotating. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../video/touch_gesture.h"

#include "../safeguards.h"

TEST_CASE("Two fingers moving together only pan")
{
	auto step = GetTwoFingerStep({100, 100}, {300, 100}, {130, 80}, {330, 80});
	CHECK(step.pan_x == Approx(30));
	CHECK(step.pan_y == Approx(-20));
	CHECK(step.scale == Approx(1));
	CHECK(step.rotate == Approx(0));
}

TEST_CASE("Two fingers moving apart only zoom")
{
	auto step = GetTwoFingerStep({100, 100}, {300, 100}, {50, 100}, {350, 100});
	CHECK(step.pan_x == Approx(0));
	CHECK(step.pan_y == Approx(0));
	CHECK(step.scale == Approx(1.5));
	CHECK(step.rotate == Approx(0));
}

TEST_CASE("Two fingers turning around their middle only rotate")
{
	/* On a screen, y grows downwards, so going from pointing right to pointing down is clockwise. */
	auto step = GetTwoFingerStep({100, 200}, {300, 200}, {200, 100}, {200, 300});
	CHECK(step.pan_x == Approx(0));
	CHECK(step.pan_y == Approx(0));
	CHECK(step.scale == Approx(1));
	CHECK(step.rotate == Approx(90));
}

TEST_CASE("Two finger rotation takes the short way across the half turn")
{
	auto step = GetTwoFingerStep({300, 201}, {100, 200}, {300, 199}, {100, 200});
	CHECK(std::abs(step.rotate) < 1);
}

TEST_CASE("Fingers almost together pan but do not zoom or rotate")
{
	auto step = GetTwoFingerStep({100, 100}, {101, 100}, {110, 100}, {110, 300});
	CHECK(step.scale == Approx(1));
	CHECK(step.rotate == Approx(0));
}
