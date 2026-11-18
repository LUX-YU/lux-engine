#pragma once

#include <lux/engine/render/resources/lighting/Resource.hpp>

#include <exec/task.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/resources/lighting/Binding.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/extensions/resource/StorageMeta.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
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
        [[nodiscard]] exec::task<RenderResult<std::vector<LightId>>> create(LightData) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> update(std::span<const DirectLightUpdate>) noexcept;
        [[nodiscard]] exec::task<RenderResult<void>> remove(LightId) noexcept;
        [[nodiscard]] RenderResult<void>
        resolve(
            const VGraphResourceDesc&,
            DirectLightingBinding,
            StorageSubscription&,
            std::span<ResourceInput>
        ) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t, ResourceUploader&, Preparation&) noexcept;
        [[nodiscard]] bool read(LightId, VDirectLight&, std::uint32_t& dense_index) const noexcept;
        [[nodiscard]] std::array<std::uint32_t, 3> counts() const noexcept;

        template <class F> void forEachShadow(F&& visitor) const noexcept
        {
            for (std::uint32_t kind = 0; kind < 3; ++kind)
            {
                for (const auto key : arrays_[kind].identities)
                {
                    const auto& entry = *entries_.find(key);
                    if (entry.shadows.enabled)
                    {
                        visitor(extension_detail::ResourceIdentity::make<LightId>(id_, key), kind, entry.shadows);
                    }
                }
            }
        }

        // Projection/mapping changes only; radiance updates still dirty GPU records independently.
        [[nodiscard]] std::uint64_t shadowRevision() const noexcept
        {
            return shadow_revision_;
        }

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
            StorageSubscription* bindings = nullptr;
        };

        struct Write final
        {
            std::uint32_t kind, position;
            std::span<const std::byte> bytes;
        };

        [[nodiscard]] exec::task<RenderResult<void>>
        write(std::span<const Write>, const std::array<std::uint32_t, 3>& counts) noexcept;
        void finishPreparation(std::uint32_t, RenderResult<void>) noexcept;
        void publish(std::uint32_t kind, std::uint32_t frame) noexcept;
        DeviceSession& device_;
        ResourceUploader& uploads_;
        FrameProgress& replica_progress_;
        std::uint32_t frame_count_;
        ResourceInstanceId id_;
        std::uint64_t preparation_revision_ = 1;
        cxx::StableSlotMap<Entry, DirectLightTag> entries_;
        std::array<LightArray, 4> arrays_;
        bool updating_ = false;
        std::uint64_t shadow_revision_ = 1;
    };

} // namespace lux::render::detail
