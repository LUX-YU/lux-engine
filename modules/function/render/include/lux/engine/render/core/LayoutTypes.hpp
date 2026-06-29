#pragma once
#include <cstdint>

namespace lux::render
{
    // ===== std430 utility types =====

    struct alignas(16) aligned16vec2 { float x, y; };
    struct alignas(16) aligned16vec3 { float x, y, z; };
    struct alignas(16) aligned16vec4 { float x, y, z, w; };
}