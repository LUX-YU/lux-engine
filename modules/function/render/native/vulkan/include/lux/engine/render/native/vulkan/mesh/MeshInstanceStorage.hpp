#pragma once

#include <exec/task.hpp>

#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/resource/MeshInstance.hpp>

namespace lux::render::vulkan
{
    [[nodiscard]] StorageFactory meshInstanceStorageFactory() noexcept;

    class MeshInstanceStorage final
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
            void (*changed)(void*, MeshInstanceId, const MeshInstanceDescription*) noexcept = nullptr;
            void reset() noexcept;

        private:
            friend class MeshInstanceStorage;
            MeshInstanceStorage* source_ = nullptr;
            std::size_t position_ = 0;
        };

        MeshInstanceStorage(DeviceSession&, StorageId) noexcept;
        ~MeshInstanceStorage() noexcept;
        MeshInstanceStorage(const MeshInstanceStorage&) = delete;
        MeshInstanceStorage& operator=(const MeshInstanceStorage&) = delete;

        [[nodiscard]] exec::task<RenderResult<MeshInstanceId>> create(MeshInstanceDescription) noexcept;
        [[nodiscard]] exec::task<
            RenderResult<std::vector<MeshInstanceId>>> create(std::vector<MeshInstanceDescription>) noexcept;
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
                    visitor(MeshInstanceId{id_, entry.key}, describe(entry));
                }
            }
        }

        [[nodiscard]] std::uint32_t recordCount() const noexcept
        {
            return live_records_;
        }

        [[nodiscard]] std::uint64_t capacity() const noexcept;

    private:
        friend StorageFactory meshInstanceStorageFactory() noexcept;
        template <class Input>
        using CreateValue = std::
            conditional_t<std::same_as<Input, MeshInstanceDescription>, MeshInstanceId, std::vector<MeshInstanceId>>;
        template <class Input> [[nodiscard]] exec::task<RenderResult<CreateValue<Input>>> createBatch(Input) noexcept;
        [[nodiscard]] RenderResult<void>
        resolve(const NativeResourcePlan&, const MeshInstanceBinding&, StorageSubscription&, std::span<StorageResource>) noexcept;
        [[nodiscard]] RenderResult<void> allocate(GpuPage&, std::size_t slots) noexcept;
        [[nodiscard]] RenderResult<void> ensureFrame(std::uint32_t slot) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t slot, UploadContext&) noexcept;
        [[nodiscard]] RenderResult<MeshInstanceGpuProperty> resolveReferences(const MeshInstanceDescription&)
            const noexcept;
        [[nodiscard]] MeshInstanceDescription describe(const Entry&) const noexcept;
        void notify(const Entry&, bool removed) noexcept;
        void publishBuffers(std::uint32_t slot) noexcept;
        void release(GpuPage&) noexcept;
        void release(PageTable&) noexcept;
        void mark(std::uint32_t frame, std::uint32_t record, std::uint8_t fields) noexcept;
        [[nodiscard]] gapi::vk::VmaBuffer& fieldBuffer(
            std::uint32_t frame,
            std::uint32_t page,
            std::size_t field
        ) noexcept;
        [[nodiscard]] VkDeviceSize fieldOffset(std::uint32_t record, std::size_t stride) const noexcept;
        DeviceSession& session_;
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
} // namespace lux::render::vulkan
