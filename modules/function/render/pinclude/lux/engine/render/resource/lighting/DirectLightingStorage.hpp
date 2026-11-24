#pragma once

#include <lux/engine/render/resource/lighting/Resource.hpp>

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
        [[nodiscard]] RenderResult<std::vector<LightId>> create(const LightData&) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> update(std::span<const DirectLightUpdate>) noexcept;
        [[nodiscard]] RenderResult<void> remove(LightId) noexcept;
        [[nodiscard]] RenderResult<void>
        resolve(
            const VGraphResourceDesc&,
            DirectLightingBinding,
            std::span<ResourceInput>
        ) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t, ResourceUploader&) noexcept;
        [[nodiscard]] std::array<std::uint32_t, 3> counts() const noexcept;
        [[nodiscard]] RenderResult<ResourceViewValue<ShadowLightSnapshot>> selectShadows() noexcept;

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

        void updateCounts() noexcept;
        void finishPreparation(const ReplicaSettlement&) noexcept;
        void publish(std::uint32_t kind, std::uint32_t frame) noexcept;
        DeviceSession& device_;
        ResourceRegistry& resources_;
        std::uint32_t frame_count_;
        ResourceInstanceId id_;
        cxx::StableSlotMap<Entry, DirectLightTag> entries_;
        std::array<LightArray, 4> arrays_;
        std::uint64_t shadow_revision_ = 1;
        RevisionedValue<ShadowLightSnapshot> shadow_selection_{};
    };

} // namespace lux::render::detail
