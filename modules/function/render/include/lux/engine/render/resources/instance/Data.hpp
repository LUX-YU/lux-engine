#pragma once


#include <string_view>
#include <lux/engine/render/core/ResourceId.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <lux/engine/render/resources/mesh/Data.hpp>
#include <lux/engine/render/resources/material/Data.hpp>
#include <span>

namespace lux::render
{
    inline constexpr std::string_view kRasterInstancesInput = "lux.render.input.raster_instances";

    struct MeshInstanceTag;
    using MeshInstanceId = ResourceId<MeshInstanceTag>;
    using TransformMatrix = std::array<float, 16>;
    inline constexpr TransformMatrix kIdentityTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    [[nodiscard]] inline bool isFiniteTransform(const TransformMatrix& value) noexcept
    {
        return std::ranges::all_of(value, [](float element) noexcept { return std::isfinite(element); });
    }

    struct MeshInstanceData final
    {
        MeshId mesh;
        MaterialId material;
        std::uint32_t submesh = 0;
        TransformMatrix transform = kIdentityTransform;
        bool render_enabled = false;
        bool cast_shadow = true;
    };

    struct TransformUpdate final
    {
        MeshInstanceId instance;
        TransformMatrix transform = kIdentityTransform;
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
    };

    struct alignas(8) MeshInstancePageGpu final
    {
        std::uint64_t transforms = 0;
        std::uint64_t properties = 0;
        std::uint64_t alive = 0;
    };

    static_assert(sizeof(MeshInstanceGpuProperty) == 20 && sizeof(MeshInstancePageGpu) == 24);
    static_assert(alignof(MeshInstanceGpuProperty) == 4);
    inline constexpr std::uint32_t kMeshInstancePageSlots = 16'384;
    inline constexpr std::uint32_t kMeshInstanceDirtySlots = 512;
    inline constexpr std::uint32_t kMeshInstanceTableAxis = 512;

    struct MeshInstanceUpdate final
    {
        MeshInstanceId instance;
        MeshInstanceData description;
    };

    // MeshInstanceUpdate also writes transform. Cross-column order is not a last-writer-wins contract.
} // namespace lux::render
