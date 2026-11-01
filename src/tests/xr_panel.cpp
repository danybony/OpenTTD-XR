/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file xr_panel.cpp Tests for the geometry of windows floating in space as panels. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../video/xr_panel_math.h"

#include "../safeguards.h"

TEST_CASE("A panel faces the eye and stays upright")
{
	using Terrain3D::Vec;
	const Vec centre{100, 200, 50}, eye{100, 0, 50};
	const auto c = XRPanel::BillboardCorners(centre, eye, 40, 20);
	for (const auto &corner : c) CHECK(corner.y == Approx(200));
	CHECK(c[0].z == Approx(60));
	CHECK(c[1].z == Approx(60));
	CHECK(c[2].z == Approx(40));
	CHECK(c[3].z == Approx(40));
	CHECK(std::abs(c[1].x - c[0].x) == Approx(40));
}

TEST_CASE("A panel seen from above still has its full size")
{
	using Terrain3D::Vec;
	const auto c = XRPanel::BillboardCorners({0, 0, 0}, {0, -100, 100}, 40, 20);
	CHECK(std::hypot(c[1].x - c[0].x, c[1].y - c[0].y, c[1].z - c[0].z) == Approx(40));
	CHECK(std::hypot(c[3].x - c[0].x, c[3].y - c[0].y, c[3].z - c[0].z) == Approx(20));
}

TEST_CASE("Panel coordinates of an axis aligned panel on the screen")
{
	const std::array<std::array<float, 2>, 4> q{{{100, 100}, {300, 100}, {300, 200}, {100, 200}}};
	float u = -1, v = -1;
	REQUIRE(XRPanel::ScreenToPanel(q, 150, 175, u, v));
	CHECK(u == Approx(0.25));
	CHECK(v == Approx(0.75));
	CHECK_FALSE(XRPanel::ScreenToPanel(q, 50, 150, u, v));
	CHECK_FALSE(XRPanel::ScreenToPanel(q, 150, 250, u, v));
}

TEST_CASE("Panel coordinates undo perspective")
{
	/* The middle of a panel in perspective is where its diagonals cross, not the average of its corners. */
	const std::array<std::array<float, 2>, 4> q{{{0, 0}, {100, 20}, {100, 80}, {0, 100}}};
	const float cross_x = 100.0f * 100 / (100 + 60);
	float u = -1, v = -1;
	REQUIRE(XRPanel::ScreenToPanel(q, cross_x, 50, u, v));
	CHECK(u == Approx(0.5));
	CHECK(v == Approx(0.5));
	REQUIRE(XRPanel::ScreenToPanel(q, 0, 0, u, v));
	CHECK(u == Approx(0).margin(1e-5));
	CHECK(v == Approx(0).margin(1e-5));
	REQUIRE(XRPanel::ScreenToPanel(q, 100, 80, u, v));
	CHECK(u == Approx(1));
	CHECK(v == Approx(1));
}
