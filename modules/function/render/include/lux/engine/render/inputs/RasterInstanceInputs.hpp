#pragma once

#include <lux/engine/render/capabilities/RenderCapability.hpp>
#include <lux/engine/render/core/ResourceId.hpp>

#include <array>
#include <lux/engine/render/inputs/IndexedTriangleMeshInputs.hpp>
#include <lux/engine/render/inputs/RasterMaterialInputs.hpp>
#include <span>

namespace lux::render
{
    struct MeshInstanceTag;
    using MeshInstanceId = ResourceId<MeshInstanceTag>;
    using TransformMatrix = std::array<float, 16>;
    inline constexpr TransformMatrix kIdentityTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    struct MeshInstanceDescription final
    {
        MeshId mesh;
        MaterialId material;
        std::uint32_t submesh = 0;
        TransformMatrix transform = kIdentityTransform;
    };

    struct MeshInstanceCreate final
    {
        CapabilityInput destination;
        MeshInstanceDescription description;
    };

    // Borrowed arrays remain immutable and at stable addresses until the terminal Response.
    // Abandon/connection loss is not completion; settle the Server and CPU producers before releasing input.
    // The Client supplies unique changed identities and finite matrices. The Server validates current generations.
    struct MeshInstancesCreate final
    {
        CapabilityInput destination;
        std::span<const MeshInstanceDescription> descriptions;
    };

    // Equal-length, position-matched arrays. A failed enqueue takes no borrow.
    struct TransformBatch final
    {
        CapabilityInput destination;
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
        std::uint32_t material_index = 0;
        std::uint32_t section_index = 0;
    };

    struct alignas(8) MeshInstancePageGpu final
    {
        std::uint64_t transforms = 0;
        std::uint64_t properties = 0;
        std::uint64_t alive = 0;
    };

    static_assert(sizeof(MeshInstanceGpuProperty) == 12 && sizeof(MeshInstancePageGpu) == 24);
    inline constexpr std::uint32_t kMeshInstancePageSlots = 16'384;
    inline constexpr std::uint32_t kMeshInstanceDirtySlots = 512;
    inline constexpr std::uint32_t kMeshInstanceTableAxis = 512;

    struct CreateMeshInstance final
    {
        static constexpr std::string_view name = "lux.render.CreateMeshInstance";
        static constexpr bool frame_scoped = true;
        using Request = MeshInstanceCreate;

        struct Response final
        {
            RenderError error{};
            MeshInstanceId instance{};
        };
    };

    struct CreateMeshInstances final
    {
        static constexpr std::string_view name = "lux.render.CreateMeshInstances";
        static constexpr bool frame_scoped = true;
        using Request = MeshInstancesCreate;

        struct Response final
        {
            RenderError error{};
            std::vector<MeshInstanceId> instances;
        };
    };

    struct UpdateTransforms final
    {
        static constexpr std::string_view name = "lux.render.UpdateTransforms";
        static constexpr bool frame_scoped = true;
        using Request = TransformBatch;

        struct Response final
        {
            RenderError error{};
            std::uint64_t sequence = 0;
            std::uint32_t changed = 0;
        };
    };

    struct MeshInstanceUpdate final
    {
        MeshInstanceId instance;
        MeshInstanceDescription description;
    };

    struct MeshInstanceBatch final
    {
        CapabilityInput destination;
        std::uint64_t sequence = 0;
        std::span<const MeshInstanceUpdate> updates;
    };

    struct UpdateMeshInstances final
    {
        static constexpr std::string_view name = "lux.render.UpdateMeshInstances";
        static constexpr bool frame_scoped = true;
        // Client supplies unique changed instances and keeps referenced Mesh/Material identities valid.
        // The entire batch validates before uploads; one CPU adoption follows accepted finite windows.
        using Request = MeshInstanceBatch;

        struct Response final
        {
            RenderError error{};
            std::uint64_t sequence = 0;
            std::uint32_t changed = 0;
        };
    };

    struct ReleaseMeshInstance final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name = "lux.render.ReleaseMeshInstance";
        static constexpr bool frame_scoped = true;
        using Request = MeshInstanceId;
        using Response = AckResponse;
    };

} // namespace lux::render
