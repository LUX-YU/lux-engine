#pragma once

#include <lux/engine/render/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/memory/DirtyRanges.hpp>
#include <lux/engine/render/resource/transform/TransformRecords.hpp>
#include <lux/engine/render/resource/transform/Resource.hpp>
#include <array>
#include <span>
#include <vector>

namespace lux::render
{
    class ResourceRegistry;
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
        [[nodiscard]] bool contains(TransformId id) const noexcept { return records_.contains(id); }
        void retainInstance(TransformId id) noexcept { records_.retainInstance(id); }
        void releaseInstance(TransformId id) noexcept { records_.releaseInstance(id); }
    private:
        static constexpr std::uint32_t kPageSlots = 16'384;

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

        void collectRetired() noexcept;
        void markDirty(std::uint32_t) noexcept;

        [[nodiscard]] RenderResult<nri::Buffer*> allocate(std::uint64_t) noexcept;
        [[nodiscard]] RenderResult<void> ensureReplica(Replica&) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t, ResourceUploader&) noexcept;
        [[nodiscard]] RenderResult<void>
        resolve(const VGraphResourceDesc&, std::span<ResourceInput>) noexcept;
        [[nodiscard]] ResourceInput binding(std::uint32_t) const noexcept;
        void finishPreparation(const ReplicaSettlement&) noexcept;
        void destroy(nri::Buffer*) noexcept;

        DeviceSession& session_;
        const FrameProgress& progress_;
        const std::uint32_t frame_count_;
        const bool paged_;
        TransformRecords records_;
        SubmissionFrontiers completed_;
        std::array<Replica, 3> replicas_;
        ResourceRegistry& resources_;
        std::uint64_t revision_ = 1;
    };
} // namespace lux::render::detail
