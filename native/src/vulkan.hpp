// SPDX-License-Identifier: GPL-3.0-or-later
//
// The window's picture through Vulkan, on the machine's own GPU (vulkan.cpp). For now it only
// shows the display's buffer: the game's drawing is still done on the CPU (draw.cpp).

#pragma once

#include <string>
#include <vector>

#include "loader.hpp"

namespace mensis::vk {

// Sets Vulkan up for the window (a Win32 HINSTANCE and HWND); false, with the reason, if it
// can't, and the window falls back to GDI.
bool open(void* instance, void* window, std::string& error);
// The picture (width x height, B, G, R, A in memory), scaled to the window's client area.
// Called on the window's thread only.
void present(const u32* pixels, u32 width, u32 height, u32 window_width, u32 window_height);
std::vector<std::string> report();  // for the summary

}  // namespace mensis::vk
