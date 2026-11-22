#pragma once

#include <lux/engine/description/Image.hpp>
#include <cstdint>
#include <span>

namespace nri { struct Buffer; }

namespace lux::render
{
    struct GeometryAttribute final
    {
        std::uint32_t location = 0, offset = 0;
        rdesc::ETextureFormat format = rdesc::ETextureFormat::UNDEFINED;
        bool operator==(const GeometryAttribute&) const noexcept = default;
    };

    struct GeometryBufferSlice final
    {
        nri::Buffer* buffer = nullptr;
        std::uint64_t offset = 0, size = 0;
        std::uint32_t stride = 0;
        bool operator==(const GeometryBufferSlice&) const noexcept = default;
    };

    struct IndexedGeometryInput final
    {
        // The supplying Resource identifies the geometry within its own generation.
        std::uint64_t item;
        GeometryBufferSlice vertices, indices;
        std::span<const GeometryAttribute> attributes;
    };
}
