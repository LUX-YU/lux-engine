#pragma once

#include <lux/engine/render/resource/lighting/Resource.hpp>

#include <exec/task.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/resource/lighting/Binding.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/StorageMeta.hpp>
#include <lux/engine/render/device/ResourceUploader.hpp>
#include <lux/engine/render/memory/DirtyRanges.hpp>

namespace lux::render::detail
{
    class DirectLightingStorage final
    {
    public:
        using Descriptor = LightResource;
        [[nodiscard]] static ResourceRegistration factory() noexcept;

        DirectLightingStorage(ResourceCreateContext&, ResourceInstanceId) noexcept;
        ~DirectLightingStorage() noexcept;
        [[nodiscard]] exec::task<RenderResult<std::vector<LightId>>> create(ReplicaMutation&, LightData) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> update(std::span<const DirectLightUpdate>) noexcept;
        [[nodiscard]] exec::task<RenderResult<void>> remove(ReplicaMutation&, LightId) noexcept;
        [[nodiscard]] RenderResult<void>
        resolve(
            const VGraphResourceDesc&,
            DirectLightingBinding,
            std::span<ResourceInput>
        ) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t, ResourceUploader&) noexcept;
        [[nodiscard]] std::array<std::uint32_t, 3> counts() const noexcept;
        [[nodiscard]] RenderResult<SnapshotValue<ShadowLightSnapshot>> selectShadows() noexcept;

    private:
        struct Entry final
        {
            std::uint32_t kind, position;
            LightShadowSettings shadows;
        };

        struct Slice final
        {
            nri::Buffer* buffer = nullptr;
            nri::Buffer* candidate = nullptr;
            std::size_t capacity = 0, candidate_capacity = 0;
            DirtyRanges dirty;
            std::vector<DirtyWordMask> selected, accepted;
        };

        struct LightArray final
        {
            std::vector<std::byte> records;
            std::vector<cxx::SlotKey<DirectLightTag>> identities;
            std::array<Slice, 3> frames;
        };

        struct Write final
        {
            std::uint32_t kind, position;
            std::span<const std::byte> bytes;
        };

        [[nodiscard]] exec::task<RenderResult<void>>
        write(ReplicaMutation&, std::span<const Write>, const std::array<std::uint32_t, 3>& counts) noexcept;
        void finishPreparation(const ReplicaSettlement&) noexcept;
        void publish(std::uint32_t kind, std::uint32_t frame) noexcept;
        DeviceSession& device_;
        ResourceRegistry& resources_;
        std::uint32_t frame_count_;
        ResourceInstanceId id_;
        std::uint64_t preparation_revision_ = 1;
        cxx::StableSlotMap<Entry, DirectLightTag> entries_;
        std::array<LightArray, 4> arrays_;
        bool updating_ = false;
        std::uint64_t shadow_revision_ = 1;
        ShadowLightSnapshot shadow_selection_;
    };

} // namespace lux::render::detail
