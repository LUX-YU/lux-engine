#pragma once

#include <lux/engine/render/graph/DrawList.hpp>
#include <vector>

namespace nri { struct Buffer; struct Texture; }
namespace lux::render { struct MeshDescription; }

namespace lux::render::detail
{
    [[nodiscard]] inline bool containsStreamRange(std::size_t size, RasterStreamRange range) noexcept
    {
        return range.first <= size && range.count <= size - range.first;
    }

    struct OwnedRasterStream final
    {
        struct MeshBuffer final
        {
            nri::Buffer* buffer;
            std::uint64_t offset, size;
            const MeshDescription* asset;
            EGraphUse use;
        };

        struct TextureRead final
        {
            const nri::Texture* image;
            GraphImageRange range;
        };

        struct MaterialRead final
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
        std::vector<TextureRead> textures;
        std::vector<MeshBuffer> mesh_buffers;
        std::vector<MaterialRead> materials;
        std::vector<TextureRead> material_images;
        std::vector<std::uint8_t> used_material_programs;
        std::uint64_t selection = 0;
        bool written = false;
    };
} // namespace lux::render::detail
