#pragma once
/**
 * @file Size2D.hpp
 * @brief 2D unsigned extent — width × height.
 *
 * A minimal, platform-agnostic size descriptor used across window,
 * render, and UI modules.  Intentionally free of any graphics API
 * dependency (no Vulkan, no GLFW).
 */

#include <cstdint>

namespace lux::common
{
    struct Size2D
    {
        uint32_t width{0};
        uint32_t height{0};
    };
} // namespace lux::common
