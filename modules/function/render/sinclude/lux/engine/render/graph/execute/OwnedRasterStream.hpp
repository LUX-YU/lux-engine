#pragma once

#include <lux/engine/render/graph/DrawList.hpp>
#include <lux/engine/render/graph/IndexedGeometry.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <vector>

namespace nri { struct Buffer; struct Texture; }

namespace lux::render::detail
{
    [[nodiscard]] inline bool containsStreamRange(std::size_t size, RasterStreamRange range) noexcept
    {
        return range.first <= size && range.count <= size - range.first;
    }

    struct OwnedRasterStream final
    {
        struct GeometryBinding final
        {
            ResourceInstanceId owner;
            std::uint64_t item;
            GeometryBufferSlice vertices, indices;
            bool operator==(const GeometryBinding&) const noexcept = default;
        };

        struct GeometryLayout final
        {
            RasterStreamRange attributes;
            std::uint32_t component_alignment;
        };

        struct Geometry final
        {
            GeometryBinding binding;
            GeometryLayout layout;
        };

        struct RecordRead final
        {
            const nri::Buffer* buffer;
            GraphBufferRange range;
            std::uint32_t program, first_image, image_count;
        };

        std::vector<RasterStreamDraw> draws;
        std::vector<RasterStreamBinding> bindings;
        std::vector<RasterStreamVertex> vertices;
        std::vector<std::byte> constants;
        std::vector<RasterStreamSection> sections;
        std::vector<RasterSampledImageRead> textures;
        std::vector<Geometry> geometry;
        std::vector<GeometryAttribute> geometry_attributes;
        std::vector<RecordRead> records;
        std::vector<RasterSampledImageRead> record_images;
        std::vector<std::uint8_t> used_record_programs;
        std::uint64_t selection = 0;
        bool written = false;
    };
} // namespace lux::render::detail
