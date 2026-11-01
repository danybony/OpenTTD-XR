/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file xr_panel_math.h Geometry of windows floating in space as panels. */

#ifndef VIDEO_XR_PANEL_MATH_H
#define VIDEO_XR_PANEL_MATH_H

#include <array>
#include <cmath>
#include "terrain3d_math.h"

namespace XRPanel {

/**
 * The corners of an upright rectangle that faces a point.
 * @param centre Middle of the rectangle.
 * @param eye Point it faces.
 * @param width Width of the rectangle.
 * @param height Height of the rectangle.
 * @return Top left, top right, bottom right and bottom left corner, as seen from \p eye.
 */
inline std::array<Terrain3D::Vec, 4> BillboardCorners(Terrain3D::Vec centre, Terrain3D::Vec eye, float width, float height)
{
	using Vec = Terrain3D::Vec;
	const Vec to_eye{eye.x - centre.x, eye.y - centre.y, eye.z - centre.z};
	const float length = std::sqrt(to_eye.x * to_eye.x + to_eye.y * to_eye.y + to_eye.z * to_eye.z);
	const Vec n = length > 0 ? Vec{to_eye.x / length, to_eye.y / length, to_eye.z / length} : Vec{0, 0, 1};

	/* Right is up x normal; straight above or below the panel any right will do. */
	Vec right{-n.y, n.x, 0};
	const float side = std::hypot(right.x, right.y);
	right = side > 1e-5f ? Vec{right.x / side, right.y / side, 0} : Vec{1, 0, 0};
	const Vec up{n.y * right.z - n.z * right.y, n.z * right.x - n.x * right.z, n.x * right.y - n.y * right.x};

	auto at = [&](float across, float along) {
		return Vec{centre.x + right.x * across + up.x * along, centre.y + right.y * across + up.y * along, centre.z + right.z * across + up.z * along};
	};
	return {at(-width / 2, height / 2), at(width / 2, height / 2), at(width / 2, -height / 2), at(-width / 2, -height / 2)};
}

/**
 * Find where a point on the screen lies on a panel, undoing its perspective.
 * @param q Corners of the panel on the screen: top left, top right, bottom right, bottom left.
 * @param x Horizontal screen position.
 * @param y Vertical screen position.
 * @param[out] u Position across the panel, from 0 on its left edge to 1 on its right edge.
 * @param[out] v Position along the panel, from 0 on its top edge to 1 on its bottom edge.
 * @return Whether the point is on the panel.
 */
inline bool ScreenToPanel(const std::array<std::array<float, 2>, 4> &q, float x, float y, float &u, float &v)
{
	/* The projective map of the unit square onto the corners, as in Heckbert's Fundamentals of Texture Mapping. */
	const float sx = q[0][0] - q[1][0] + q[2][0] - q[3][0], sy = q[0][1] - q[1][1] + q[2][1] - q[3][1];
	const float dx1 = q[1][0] - q[2][0], dx2 = q[3][0] - q[2][0], dy1 = q[1][1] - q[2][1], dy2 = q[3][1] - q[2][1];
	const float det = dx1 * dy2 - dx2 * dy1;
	if (std::abs(det) < 1e-6f) return false;
	const float g = (sx * dy2 - dx2 * sy) / det, h = (dx1 * sy - sx * dy1) / det;
	const float a = q[1][0] - q[0][0] + g * q[1][0], b = q[3][0] - q[0][0] + h * q[3][0], c = q[0][0];
	const float d = q[1][1] - q[0][1] + g * q[1][1], e = q[3][1] - q[0][1] + h * q[3][1], f = q[0][1];

	/* Inverting it needs the adjugate of that matrix. */
	const float A = e - f * h, B = c * h - b, C = b * f - c * e, D = f * g - d, E = a - c * g, F = c * d - a * f, G = d * h - e * g, H = b * g - a * h, I = a * e - b * d;
	const float w = G * x + H * y + I;
	if (std::abs(w) < 1e-6f) return false;
	u = (A * x + B * y + C) / w;
	v = (D * x + E * y + F) / w;
	return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

} // namespace XRPanel

#endif /* VIDEO_XR_PANEL_MATH_H */
