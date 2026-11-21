#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/memory/DirtyRanges.hpp>
#include <lux/engine/render/resource/transform/TransformSlotRetirement.hpp>
#include <lux/engine/render/resources/transform/Resource.hpp>
#include <array>
#include <optional>
#include <span>
#include <vector>

namespace lux::render
{
    struct ResourceCreateContext;
    struct ResourceRegistration;
}

namespace lux::render::detail
{
    class DeviceSession;
    class FrameProgress;
    class TransformStorage final
    {
    public:
        using Descriptor = TransformResource;

        TransformStorage(ResourceCreateContext&, ResourceInstanceId) noexcept;
        ~TransformStorage() noexcept;
        [[nodiscard]] static ResourceRegistration factory() noexcept;
        [[nodiscard]] RenderResult<std::vector<TransformId>> create(std::span<const TransformMatrix>) noexcept;
        [[nodiscard]] RenderResult<void> update(std::span<const TransformRecordUpdate>) noexcept;
        [[nodiscard]] RenderResult<void> release(TransformId) noexcept;
        [[nodiscard]] std::optional<std::uint32_t> gpuSlot(TransformId) const noexcept;
        void retainInstance(TransformId) noexcept;
        void releaseInstance(TransformId) noexcept;
    private:
        [[nodiscard]] bool canCreate(std::size_t count) const noexcept;
        [[nodiscard]] TransformId insert(const TransformMatrix&) noexcept;
        static constexpr std::uint32_t kPageSlots = 16'384;

        struct Record final
        {
            std::uint32_t gpu_slot;
            std::uint32_t references = 0;
        };

        struct Replica final
        {
            nri::Buffer* binding = nullptr;
            nri::Buffer* candidate = nullptr;
            std::vector<nri::Buffer*> pages;
            std::vector<nri::Buffer*> addressed;
            std::vector<std::uint64_t> addresses;
            DirtyRanges dirty;
            std::vector<DirtyWordMask> selected, staged;
            std::uint64_t address_revision = 0;
            bool table_dirty = false;
            bool table_staged = false;
        };

        [[nodiscard]] Record* find(TransformId) noexcept;
        [[nodiscard]] const Record* find(TransformId) const noexcept;
        void collectRetired() noexcept;
        void markDirty(std::uint32_t) noexcept;

        [[nodiscard]] RenderResult<nri::Buffer*> allocate(std::uint64_t) noexcept;
        [[nodiscard]] RenderResult<void> ensureReplica(Replica&) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t, ResourceUploader&) noexcept;
        [[nodiscard]] RenderResult<void>
        resolve(const VGraphResourceDesc&, StorageSubscription&, std::span<ResourceInput>) noexcept;
        [[nodiscard]] ResourceInput binding(std::uint32_t) const noexcept;
        void finishPreparation(const ReplicaSettlement&) noexcept;
        void destroy(nri::Buffer*) noexcept;

        ResourceInstanceId id_;
        DeviceSession& session_;
        const FrameProgress& progress_;
        const std::uint32_t frame_count_;
        const bool paged_;
        cxx::SlotMap<Record, TransformTag> records_;
        std::vector<TransformMatrix> matrices_{2};
        std::vector<std::uint32_t> free_slots_{1, 0};
        TransformSlotRetirement retirement_;
        SubmissionFrontiers completed_;
        std::array<Replica, 3> replicas_;
        StorageSubscription* subscriptions_ = nullptr;
        std::uint64_t revision_ = 1;
    };
} // namespace lux::render::detail
