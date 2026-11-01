/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file xr_panels.h Windows floating in space as panels, around the tabletop view. */

#ifndef XR_PANELS_H
#define XR_PANELS_H

#include "core/geometry_type.hpp"
#include <array>
#include <vector>

/** One window to draw as a panel, from its back to the front. */
struct XRPanelDraw {
	std::array<std::array<float, 4>, 4> corners; ///< Clip space corners: top left, top right, bottom left, bottom right.
	std::array<float, 4> uv; ///< The window in the video buffer: left, top, right, bottom in texture coordinates.
	bool has_view; ///< The window holds a 3D view, which shows through its holes from the same place in the 3D layer.
	bool grip; ///< This is a handle to drag the panel by, drawn in plain colour rather than from the buffer.
};

#ifdef WITH_3D_PROOF
bool IsXRPanelsActive();
void SetXRPanels(bool active);
void SetXRPanelsRoomAnchored(bool room);
void UpdateXRPanels();
void LogXRPanels();
int GetXRAtlasExtraHeight();
int GetXRDisplayHeight();
const std::vector<XRPanelDraw> &GetXRPanelDraws();
Point MapScreenToXRPanels(int x, int y);
bool StartXRPanelDrag(int x, int y);
bool IsXRPanelDragging();
void MoveXRPanelDrag(int x, int y);
void EndXRPanelDrag();
#else
inline bool IsXRPanelsActive() { return false; }
inline void UpdateXRPanels() {}
inline int GetXRAtlasExtraHeight() { return 0; }
inline Point MapScreenToXRPanels(int x, int y) { return {x, y}; }
inline bool StartXRPanelDrag(int, int) { return false; }
inline bool IsXRPanelDragging() { return false; }
inline void MoveXRPanelDrag(int, int) {}
inline void EndXRPanelDrag() {}
#endif

#endif /* XR_PANELS_H */
