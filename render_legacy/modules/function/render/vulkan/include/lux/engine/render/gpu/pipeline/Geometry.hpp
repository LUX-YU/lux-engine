#pragma once
#include <cstdint>

namespace lux::render
{
    enum class EGeometryType : uint32_t
    {
        MESH = 0,
        INSTANCED_MESH,
        POINT_CLOUD,
        LINE_SEGMENTS,
        VOLUMETRIC,
        PARTICLES,
        IMPLICIT_SURFACE,
        COUNT
    };
}
