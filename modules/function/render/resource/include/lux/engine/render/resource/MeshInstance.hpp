#pragma once

#include <lux/engine/render/resource/Material.hpp>
#include <lux/engine/render/resource/Mesh.hpp>
#include <optional>
#include <tuple>

namespace lux::render
{
    struct MeshInstanceTag;
    using MeshInstanceId = StorageResourceId<MeshInstanceTag>;
    using TransformMatrix = std::array<float, 16>;
    inline constexpr TransformMatrix kIdentityTransform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    inline constexpr StorageNameView kMeshInstanceStorageType("lux.render.MeshInstanceStorage");

    struct MeshInstanceDescription final
    {
        MeshId mesh;
        MaterialId material;
        std::uint32_t submesh = 0;
        TransformMatrix transform = kIdentityTransform;
    };

    struct MeshInstanceCreate final
    {
        StorageId storage;
        MeshInstanceDescription description;
    };

    struct TransformUpdate final
    {
        MeshInstanceId instance;
        TransformMatrix transform = kIdentityTransform;
    };

    struct TransformBatch final
    {
        StorageId storage;
        std::uint64_t sequence = 0; // Caller revision echoed exactly, never acknowledges later dirty state.
        std::vector<TransformUpdate> updates;
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

    enum class EMeshInstanceBinding : std::uint8_t
    {
        TRANSFORMS,
        PROPERTIES,
        ALIVE
    };

    struct MeshInstanceBinding final
    {
        EMeshInstanceBinding role = EMeshInstanceBinding::TRANSFORMS;
        bool operator==(const MeshInstanceBinding&) const noexcept = default;
    };

    struct CreateMeshInstance final
    {
        static constexpr std::string_view name = "lux.render.CreateMeshInstance";
        using Request = MeshInstanceCreate;
        using Response = RenderResult<MeshInstanceId>;
    };

    struct UpdateTransforms final
    {
        static constexpr std::string_view name = "lux.render.UpdateTransforms";
        using Request = TransformBatch;
        using Response = RenderResult<TransformApplied>;
    };

    struct MeshInstanceUpdate final
    {
        MeshInstanceId instance;
        MeshInstanceDescription description;
    };

    struct MeshInstanceBatch final
    {
        StorageId storage;
        std::uint64_t sequence = 0;
        std::vector<MeshInstanceUpdate> updates;
    };

    struct UpdateMeshInstances final
    {
        static constexpr std::string_view name = "lux.render.UpdateMeshInstances";
        // Client supplies unique changed instances and keeps referenced Mesh/Material identities valid.
        // The entire batch validates before a single GPU submission and CPU adoption.
        using Request = MeshInstanceBatch;
        using Response = RenderResult<TransformApplied>;
    };

    struct ReleaseMeshInstance final
    {
        static constexpr std::string_view name = "lux.render.ReleaseMeshInstance";
        using Request = MeshInstanceId;
        using Response = RenderResult<Ack>;
    };

    class MeshInstanceStorage final
    {
    public:
        using Config = void;
        static inline const StorageDescriptor
            description{kMeshInstanceStorageType, 0, 0, cppType<MeshInstanceBinding>()};

        using Ops = void;
    };
} // namespace lux::render
