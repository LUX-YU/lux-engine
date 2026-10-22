#pragma once

#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/StorageCreateContext.hpp>

#include <exec/task.hpp>

#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/StorageFactory.hpp>
#include <lux/engine/render/storage/RasterMeshInstances.hpp>

namespace lux::render::detail
{

    class RasterMeshInstanceStorage final
    {
        struct GpuPage;
        struct Page;
        struct PageTable;
        struct FrameData;

        struct Entry final
        {
            MeshId mesh;
            MaterialId material;
            std::uint32_t submesh = 0;
            cxx::SlotKey<MeshInstanceTag> key;
        };

    public:
        using Descriptor = RasterMeshInstanceStorageDescriptor;
        [[nodiscard]] static StorageFactory factory() noexcept;

        struct Selection final
        {
            MeshId mesh;
            MaterialId material;
            std::uint32_t submesh = 0;
        };

        // A borrowed change node owned by the interested Feature's actual Context data. Storage
        // reports identity changes; only the Feature decides selection and indirect membership.
        class Observer final
        {
        public:
            Observer() noexcept = default;
            ~Observer() noexcept;
            Observer(const Observer&) = delete;
            Observer& operator=(const Observer&) = delete;
            void* receiver = nullptr;
            void (*changed)(void*, MeshInstanceId, const Selection*) noexcept = nullptr;
            void reset() noexcept;

        private:
            friend class RasterMeshInstanceStorage;
            RasterMeshInstanceStorage* source_ = nullptr;
            std::size_t position_ = 0;
        };

        RasterMeshInstanceStorage(StorageCreateContext&, StorageId) noexcept;
        ~RasterMeshInstanceStorage() noexcept;
        RasterMeshInstanceStorage(const RasterMeshInstanceStorage&) = delete;
        RasterMeshInstanceStorage& operator=(const RasterMeshInstanceStorage&) = delete;

        [[nodiscard]] exec::task<RenderResult<MeshInstanceId>> create(MeshInstanceDescription) noexcept;
        [[nodiscard]] exec::task<
            RenderResult<std::vector<MeshInstanceId>>> create(std::span<const MeshInstanceDescription>) noexcept;
        [[nodiscard]] exec::task<RenderResult<TransformApplied>> update(TransformBatch) noexcept;
        [[nodiscard]] exec::task<RenderResult<TransformApplied>> update(MeshInstanceBatch) noexcept;
        [[nodiscard]] exec::task<RenderResult<Ack>> release(MeshInstanceId) noexcept;
        void observe(Observer&) noexcept;

        template <class Visitor> void forEach(Visitor&& visitor) const noexcept
        {
            for (const auto& entry : entries_)
            {
                if (!entry.key.isNull())
                {
                    visitor(MeshInstanceId{id_, entry.key}, selection(entry));
                }
            }
        }

        [[nodiscard]] std::uint32_t recordCount() const noexcept
        {
            return live_records_;
        }

        [[nodiscard]] std::uint64_t capacity() const noexcept;

    private:
        template <class Input>
        using CreateValue = std::
            conditional_t<std::same_as<Input, MeshInstanceDescription>, MeshInstanceId, std::vector<MeshInstanceId>>;
        template <class Input> [[nodiscard]] exec::task<RenderResult<CreateValue<Input>>> createBatch(Input) noexcept;
        [[nodiscard]] RenderResult<void>
        resolve(const CompiledResource&, const MeshInstanceBinding&, StorageSubscription&, std::span<ResourceInput>) noexcept;
        [[nodiscard]] RenderResult<void> allocate(GpuPage&, std::size_t slots) noexcept;
        [[nodiscard]] RenderResult<void> ensureFrame(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t slot, ResourceUploader&) noexcept;
        [[nodiscard]] RenderResult<MeshInstanceGpuProperty> resolveReferences(const MeshInstanceDescription&)
            const noexcept;
        [[nodiscard]] Selection selection(const Entry&) const noexcept;
        void notify(const Entry&, bool removed) noexcept;
        void publishBuffers(std::uint32_t slot) noexcept;
        void adoptFrameBuffers(std::uint32_t slot) noexcept;
        void finishFramePreparation(std::uint32_t slot, RenderResult<void>) noexcept;
        void release(GpuPage&) noexcept;
        void release(PageTable&) noexcept;
        void mark(std::uint32_t frame, std::uint32_t record, std::uint8_t fields) noexcept;
        [[nodiscard]] nri::Buffer* fieldBuffer(std::uint32_t frame, std::uint32_t page, std::size_t field) noexcept;
        [[nodiscard]] std::uint64_t fieldOffset(std::uint32_t record, std::size_t stride) const noexcept;
        DeviceSession& session_;
        ResourceUploader& uploads_;
        FrameExecution& frame_execution_;
        ResourceRegistry& resources_;

        StorageId id_;
        const bool paged_;
        bool updating_ = false;
        std::uint32_t live_records_ = 0;
        cxx::SlotMap<Entry, MeshInstanceTag> entries_;
        std::vector<std::unique_ptr<Page>> pages_;
        std::unique_ptr<FrameData> frames_;
        std::array<StorageSubscription*, 3> bindings_{};
        std::vector<Observer*> observers_;
    };
} // namespace lux::render::detail
