#pragma once


#include <string_view>
#include <lux/engine/render/resource/ResourceId.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <lux/engine/render/resource/mesh/Data.hpp>
#include <lux/engine/render/resource/material/Data.hpp>
#include <lux/engine/render/resource/transform/Data.hpp>
#include <span>
#include <type_traits>
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
        TransformRef transform{};
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
        TransformId transform_id{};
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

    // Each view has two streams: positive determinant first, reflected second. Consumers use the
    // matching raster front face. Singular transforms do not produce drawable surfaces.
    inline constexpr std::uint32_t kMeshWindingCount = 2;

    [[nodiscard]] constexpr std::uint64_t meshVisibleStride(std::uint64_t capacity) noexcept
    {
        return ((capacity + 68) & ~std::uint64_t{63}) * 4;
    }

    // The visibility kernels address these offsets in uint words.
    struct MeshDrawGroupRecord final
    {
        std::uint32_t index_count = 0, candidate_count = 0, first_index = 0;
        std::int32_t vertex_offset = 0;
        std::uint32_t candidate_offset = 0, visible_offset = 0, visible_stride = 0, view_count = 1;
        MeshGpuBounds bounds;
        bool operator==(const MeshDrawGroupRecord&) const noexcept = default;
    };

    static_assert(sizeof(MeshDrawGroupRecord) == 64 && offsetof(MeshDrawGroupRecord, bounds) == 32);
    static_assert(std::is_standard_layout_v<MeshDrawGroupRecord> && std::is_trivially_copyable_v<MeshDrawGroupRecord>);

    struct MeshDrawTopology final
    {
        struct MeshProgram final
        {
            std::uint32_t mesh; // Index into meshes.
            MaterialProgramId program;
            bool operator==(const MeshProgram&) const noexcept = default;
        };

        struct Surface final
        {
            std::uint32_t source; // Index into mesh_programs.
            MaterialId material;
        };

        // The scene owns this topology. CPU algorithms borrow it while selecting draws and shadow visibility.
        std::vector<MeshId> meshes;
        std::vector<MeshProgram> mesh_programs;
        std::vector<std::uint32_t> group_sources;
        std::vector<Surface> surfaces;
        // One GPU record per group, in the same order. Empty scenes publish a zero-draw sentinel.
        std::vector<MeshDrawGroupRecord> draw_groups;
        std::array<std::uint32_t, 6> camera_dispatch{};
        // Selection revisions for ordered program inputs and material reads. Zero disables reuse.
        std::uint64_t program_use_revision = 0, surface_revision = 0;
        // Visibility consumes draw_groups and camera_dispatch. Raster commands depend on the ordered
        // group sources and visible-member ranges. Candidate IDs alone do not change either layout.
        std::uint64_t visibility_revision = 0, raster_layout_revision = 0;
    };
} // namespace lux::render
