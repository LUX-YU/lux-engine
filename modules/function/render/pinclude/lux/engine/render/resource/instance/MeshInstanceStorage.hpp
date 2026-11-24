#pragma once

#include <lux/engine/render/resource/instance/Resource.hpp>
#include <lux/engine/render/effect/implementations/mesh_common/DrawBuckets.hpp>

#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/ResourceCreateContext.hpp>


#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/resource/instance/Binding.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/StorageMeta.hpp>

namespace lux::render::detail
{

    class TransformStorage;

    class MeshInstanceStorage final
    {
        struct GpuPage;
        struct Page;
        struct PageTable;
        struct FrameData;
        struct InputSources;

        struct InstanceRecord final
        {
            MeshId mesh;
            TransformRef transform;
        };

    public:
        using Descriptor = InstanceResource;
        [[nodiscard]] static ResourceRegistration factory() noexcept;

        MeshInstanceStorage(ResourceCreateContext&, ResourceInstanceId, TransformStorage&) noexcept;
        ~MeshInstanceStorage() noexcept;
        MeshInstanceStorage(const MeshInstanceStorage&) = delete;
        MeshInstanceStorage& operator=(const MeshInstanceStorage&) = delete;

        [[nodiscard]] RenderResult<void> create(std::span<const MeshInstanceData>, std::span<MeshInstanceId>) noexcept;
        [[nodiscard]] RenderResult<void> update(std::span<const MeshInstanceUpdate>) noexcept;
        [[nodiscard]] RenderResult<void> release(MeshInstanceId) noexcept;

        [[nodiscard]] std::uint64_t capacity() const noexcept;

    private:
        [[nodiscard]] RenderResult<void> refreshDrawGroups() noexcept;
        [[nodiscard]] RenderResult<ResourceViewValue<MeshDrawTopology>> selectDrawTopology() noexcept;
        [[nodiscard]] ResourceInput pagedRoot(std::uint32_t frame) const noexcept;
        [[nodiscard]] RenderResult<ResourceProjection> describeGraphImport(std::uint32_t projection) const noexcept;
        [[nodiscard]] RenderResult<void> resolveGraphImport(
            std::uint32_t projection, const VGraphResourceDesc&, std::span<ResourceInput>
        ) noexcept;
        [[nodiscard]] RenderResult<void> allocate(GpuPage&, std::size_t slots) noexcept;
        [[nodiscard]] RenderResult<void> ensureFrame(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t slot, ResourceUploader&) noexcept;
        [[nodiscard]] RenderResult<void>
        copyProperties(std::uint32_t first, std::uint32_t frame, std::span<MeshInstanceGpuProperty>) const noexcept;
        struct ResolvedInstance final
        {
            MeshInstanceGpuProperty property;
            MaterialProgramId program;
        };

        [[nodiscard]] RenderResult<ResolvedInstance>
        resolveReferences(const MeshInstanceData&, InputSources&) const noexcept;
        [[nodiscard]] RenderResult<bool>
        publishFrameBindings(std::uint32_t frame, std::span<const GraphBufferPublication> groups) noexcept;
        [[nodiscard]] RenderResult<bool> prepareGroupInputs(std::uint32_t frame, ResourceUploader&) noexcept;
        void finishFramePreparation(const ReplicaSettlement&) noexcept;
        void release(GpuPage&) noexcept;
        void release(PageTable&) noexcept;
        void mark(std::uint32_t frame, std::uint32_t record, std::uint8_t fields) noexcept;
        [[nodiscard]] nri::Buffer* fieldBuffer(std::uint32_t frame, std::uint32_t page, std::size_t field) noexcept;
        [[nodiscard]] std::uint64_t fieldOffset(std::uint32_t record, std::size_t stride) const noexcept;
        DeviceSession& session_;
        std::uint32_t frame_count_;
        ResourceRegistry& resources_;
        TransformStorage& transforms_;

        ResourceInstanceId id_;
        std::uint64_t selection_revision_ = 1;
        MeshGrouping grouping_;
        const bool paged_;
        cxx::SlotMap<InstanceRecord, MeshInstanceTag> entries_;
        std::vector<ResolvedInstance> resolved_instances_;
        std::vector<std::unique_ptr<Page>> pages_;
        std::unique_ptr<FrameData> frames_;
    };
} // namespace lux::render::detail
