#pragma once


#include <string_view>
#include <lux/engine/render/core/ResourceId.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <lux/engine/render/resources/mesh/Data.hpp>
#include <lux/engine/render/resources/material/Data.hpp>
#include <lux/engine/render/resources/transform/Data.hpp>
#include <span>
#include <vector>

namespace lux::render
{
    inline constexpr std::string_view kRasterInstancesInput = "lux.render.input.raster_instances";

    struct MeshInstanceTag;
    using MeshInstanceId = ResourceId<MeshInstanceTag>;
    struct MeshInstanceData final
    {
        MeshId mesh;
        MaterialId material;
        std::uint32_t submesh = 0;
        TransformId transform_id{};
        bool render_enabled = false;
        bool cast_shadow = true;
    };

    // The live GPU fields used by the current Mesh Feature. No previous-transform or culling
    // stream is allocated until an actual writer and shader consume it.
    struct MeshInstanceGpuProperty final
    {
        std::uint32_t mesh_index = 0;
        std::uint32_t material_buffer = UINT32_MAX;
        std::uint32_t material_record = 0;
        std::uint32_t section_index = 0;
        std::uint32_t render_flags = 0; // bit 0: rendering enabled; bit 1: casts shadows.
        std::uint32_t transform_record = 0;
    };

    struct alignas(8) MeshInstancePageGpu final
    {
        std::uint64_t properties = 0;
        std::uint64_t alive = 0;
    };

    static_assert(sizeof(MeshInstanceGpuProperty) == 24 && sizeof(MeshInstancePageGpu) == 16);
    static_assert(alignof(MeshInstanceGpuProperty) == 4);
    inline constexpr std::uint32_t kMeshInstancePageSlots = 16'384;
    inline constexpr std::uint32_t kMeshInstanceDirtySlots = 512;
    inline constexpr std::uint32_t kMeshInstanceTableAxis = 512;

    struct MeshInstanceUpdate final
    {
        MeshInstanceId instance;
        MeshInstanceData description;
    };

    struct MeshInstanceSnapshot final
    {
        struct Selection final
        {
            MeshInstanceId instance;
            MeshId mesh;
            MaterialId material;
            std::uint32_t submesh;
            MaterialProgramId program;
            bool render_enabled;
        };

        struct Section final
        {
            MeshId mesh;
            std::uint32_t submesh;
            MeshArenaSection arena;
        };

        std::vector<Selection> instances;
        std::vector<Section> sections; // Sorted by MeshId and submesh.
        std::uint64_t revision = 0;
    };
} // namespace lux::render
