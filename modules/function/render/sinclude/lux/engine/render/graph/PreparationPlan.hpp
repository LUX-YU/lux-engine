#pragma once

#include <lux/engine/object/CodeLease.hpp>
#include <lux/engine/render/extensions/execution/InstanceDataFactory.hpp>

namespace nri { struct Buffer; struct Texture; }

namespace lux::render::detail
{
    struct LinkedInlineBlock final
    {
        std::uint32_t pass;
        std::size_t bytes;
    };

    struct LinkedPreparationProducer final
    {
        // The producer's config payload and instantiation thunk may originate in this module.
        object::CodeLease code;
        InstanceDataPrototype prototype;
        std::vector<std::uint32_t> inputs;
        std::vector<std::uint32_t> outputs;
        std::vector<LinkedInlineBlock> inline_blocks;
        std::vector<std::uint32_t> raster_streams;
        std::vector<std::uint32_t> buffers;
        std::vector<std::uint32_t> snapshots;
        // The Effect execution borrow protects this owner-side source through every Job.
        std::span<const std::byte> node_state;
        std::vector<std::uint32_t> program_snapshots;
    };

    struct OwnedRasterStream final
    {
        struct MeshBuffer final
        {
            nri::Buffer* buffer;
            std::uint64_t offset, size;
            const MeshDescription* asset;
            EGraphUse use;
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
        std::vector<const nri::Texture*> textures;
        std::vector<MeshBuffer> mesh_buffers;
        std::vector<MaterialRead> materials;
        std::vector<const nri::Texture*> material_images;
        std::vector<std::uint8_t> used_material_programs;
        std::uint64_t selection = 0;
        bool written = false;
    };

    struct PreparationPlan final
    {
        struct RasterBindings final
        {
            std::uint32_t textures = kGraphInvalidIndex, meshes = kGraphInvalidIndex;
            std::uint32_t materials = kGraphInvalidIndex;
        };

        struct Buffer final
        {
            std::uint32_t resource;
            std::uint64_t minimum_bytes, maximum_bytes;
            bool upload;
            bool used;
        };

        struct Snapshot final
        {
            cxx::basic_type_info type;
            std::uint32_t binding;
        };

        std::vector<std::size_t> cpu_data_bytes;
        std::vector<Buffer> buffers;
        std::vector<Snapshot> snapshots;
        std::vector<RasterBindings> raster_bindings; // Final pass slot to canonical Job bindings.
        std::vector<LinkedPreparationProducer> producers;
    };
} // namespace lux::render::detail
