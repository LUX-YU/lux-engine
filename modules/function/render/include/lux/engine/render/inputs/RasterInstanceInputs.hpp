#pragma once

#include <lux/engine/render/frame/FrameData.hpp>

#include <string_view>
#include <lux/engine/render/core/ResourceId.hpp>

#include <array>
#include <cstddef>
#include <lux/engine/render/inputs/IndexedTriangleMeshInputs.hpp>
#include <lux/engine/render/inputs/RasterMaterialInputs.hpp>
#include <span>

namespace lux::render
{
    inline constexpr std::string_view kRasterInstancesInput = "lux.render.input.raster_instances";

    struct MeshInstanceTag;
    using MeshInstanceId = ResourceId<MeshInstanceTag>;
    using TransformMatrix = std::array<float, 16>;
    inline constexpr TransformMatrix kIdentityTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    struct MeshInstanceData final
    {
        MeshId mesh;
        MaterialId material;
        std::uint32_t submesh = 0;
        TransformMatrix transform = kIdentityTransform;
        bool render_enabled = false;
        bool cast_shadow = true;
    };

    // Equal-length, position-matched arrays. Client supplies unique changed identities and finite matrices.
    // Keep all arrays immutable until terminal completion; failed enqueue takes no borrow.
    // Abandon/connection loss requires Host settlement before releasing source memory.
    struct TransformData final
    {
        std::uint64_t sequence = 0; // Caller revision echoed exactly, never acknowledges later dirty state.
        std::span<const MeshInstanceId> instances;
        std::span<const TransformMatrix> transforms;
    };

    struct TransformApplied final
    {
        std::uint64_t sequence = 0;
        std::uint32_t changed = 0;
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

    struct MeshInstanceUpdates final
    {
        std::uint64_t sequence = 0;
        std::span<const MeshInstanceUpdate> updates;
    };

    template <> struct FrameDataLayout<TransformData> final
    {
        struct Stored final
        {
            std::uint64_t sequence = 0;
            FrameDataRange<MeshInstanceId> instances;
            FrameDataRange<TransformMatrix> transforms;
        };

        static RenderResult<TransformData> read(const FrameDataView& frame, const Stored& input) noexcept
        {
            const auto instances = frame.read(input.instances);
            if (!instances)
            {
                return cxx::unexpected<RenderError>(instances.error());
            }
            const auto transforms = frame.read(input.transforms);
            if (!transforms)
            {
                return cxx::unexpected<RenderError>(transforms.error());
            }
            return TransformData{input.sequence, *instances, *transforms};
        }
    };

    template <> struct FrameDataLayout<MeshInstanceUpdates> final
    {
        struct Stored final
        {
            std::uint64_t sequence = 0;
            FrameDataRange<MeshInstanceUpdate> updates;
        };

        static RenderResult<MeshInstanceUpdates> read(const FrameDataView& frame, const Stored& input) noexcept
        {
            const auto updates = frame.read(input.updates);
            if (!updates)
            {
                return cxx::unexpected<RenderError>(updates.error());
            }
            return MeshInstanceUpdates{input.sequence, *updates};
        }
    };

} // namespace lux::render
