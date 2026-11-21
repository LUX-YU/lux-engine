#pragma once

#include <lux/engine/render/resources/instance/Resource.hpp>

#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/extensions/resource/ResourceCreateContext.hpp>

#include <exec/task.hpp>
#include <type_traits>

#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/resources/instance/Binding.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/extensions/resource/StorageMeta.hpp>

namespace lux::render::detail
{

    class TransformStorage;

    class RasterMeshInstanceStorage final
    {
        struct GpuPage;
        struct Page;
        struct PageTable;
        struct FrameData;

        struct Entry final
        {
            cxx::SlotKey<MeshInstanceTag> key;
        };

        struct InstanceRecord final
        {
            MeshId mesh;
            MaterialId material;
            TransformId transform;
            std::uint32_t submesh = 0;
        };

        static_assert(std::is_standard_layout_v<InstanceRecord>);
        static_assert(std::is_trivially_destructible_v<InstanceRecord>);

    public:
        using Descriptor = InstanceResource;
        [[nodiscard]] static ResourceRegistration factory() noexcept;

        RasterMeshInstanceStorage(ResourceCreateContext&, ResourceInstanceId, TransformStorage&) noexcept;
        ~RasterMeshInstanceStorage() noexcept;
        RasterMeshInstanceStorage(const RasterMeshInstanceStorage&) = delete;
        RasterMeshInstanceStorage& operator=(const RasterMeshInstanceStorage&) = delete;

        [[nodiscard]] exec::task<RenderResult<MeshInstanceId>> create(ReplicaMutation&, MeshInstanceData) noexcept;
        [[nodiscard]] exec::task<
            RenderResult<std::vector<MeshInstanceId>>>
            create(ReplicaMutation&, std::span<const MeshInstanceData>) noexcept;
        [[nodiscard]] RenderResult<void> update(std::span<const MeshInstanceUpdate>) noexcept;
        [[nodiscard]] exec::task<RenderResult<Ack>> release(ReplicaMutation&, MeshInstanceId) noexcept;
        [[nodiscard]] std::uint32_t recordCount() const noexcept
        {
            return live_records_;
        }

        [[nodiscard]] std::uint64_t capacity() const noexcept;

    private:
        [[nodiscard]] RenderResult<std::uint64_t> captureMeshSelections(MeshInstanceSnapshot&) const noexcept;
        [[nodiscard]] ResourceInput pagedRoot(std::uint32_t frame) noexcept;
        [[nodiscard]] RenderResult<StorageGraphImport> describeGraphImport(std::uint32_t projection) const noexcept;
        [[nodiscard]] RenderResult<void> resolveGraphImport(
            std::uint32_t projection, const VGraphResourceDesc&, std::span<ResourceInput>
        ) noexcept;
        template <class Input>
        using CreateValue =
            std::conditional_t<std::same_as<Input, MeshInstanceData>, MeshInstanceId, std::vector<MeshInstanceId>>;
        template <class Input> [[nodiscard]] exec::task<RenderResult<CreateValue<Input>>>
        createBatch(ReplicaMutation&, Input) noexcept;
        [[nodiscard]] RenderResult<void>
        resolve(
            const VGraphResourceDesc&,
            const MeshInstanceBinding&,
            std::span<ResourceInput>
        ) noexcept;
        [[nodiscard]] RenderResult<void> allocate(GpuPage&, std::size_t slots) noexcept;
        [[nodiscard]] RenderResult<void> ensureFrame(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t slot, ResourceUploader&) noexcept;
        [[nodiscard]] RenderResult<MeshInstanceGpuProperty>
        resolveReferences(const MeshInstanceData&, std::uint32_t frame) const noexcept;
        [[nodiscard]] const InstanceRecord& instanceRecord(const Entry&) const noexcept;
        void invalidateSelection() noexcept;
        void publishBuffers(std::uint32_t slot) noexcept;
        void adoptFrameBuffers(std::uint32_t slot) noexcept;
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
        std::uint64_t preparation_revision_ = 1;
        std::uint64_t selection_revision_ = 1;
        const bool paged_;
        bool updating_ = false;
        std::uint32_t live_records_ = 0;
        cxx::SlotMap<Entry, MeshInstanceTag> entries_;
        std::vector<std::unique_ptr<Page>> pages_;
        std::unique_ptr<FrameData> frames_;
    };
} // namespace lux::render::detail
