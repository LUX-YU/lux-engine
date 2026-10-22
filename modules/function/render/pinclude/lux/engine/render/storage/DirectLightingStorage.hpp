#pragma once

#include <exec/task.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/memory/DirtyRanges.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/resource/StorageFactory.hpp>
#include <lux/engine/render/storage/DirectLighting.hpp>
#include <lux/engine/render/upload/ResourceUploader.hpp>

namespace lux::render::detail
{
    class DirectLightingStorage final
    {
    public:
        using Descriptor = DirectLightingStorageDescriptor;
        [[nodiscard]] static StorageFactory factory() noexcept;

        DirectLightingStorage(StorageCreateContext&, StorageId) noexcept;
        ~DirectLightingStorage() noexcept;
        [[nodiscard]] exec::task<RenderResult<std::vector<LightId>>> create(CreateDirectLights::Request) noexcept;
        [[nodiscard]] exec::task<RenderResult<std::uint32_t>> update(UpdateDirectLights::Request) noexcept;
        [[nodiscard]] exec::task<RenderResult<Ack>> remove(LightId) noexcept;
        [[nodiscard]] RenderResult<void>
        resolve(const CompiledResource&, DirectLightingBinding, StorageSubscription&, std::span<ResourceInput>) noexcept;
        [[nodiscard]] RenderResult<bool> prepareFrame(std::uint32_t, ResourceUploader&) noexcept;
        [[nodiscard]] bool read(LightId, VDirectLight&, std::uint32_t& dense_index) const noexcept;
        [[nodiscard]] std::array<std::uint32_t, 3> counts() const noexcept;

        [[nodiscard]] std::uint64_t revision() const noexcept
        {
            return revision_;
        }

    private:
        struct Entry final
        {
            std::uint32_t kind, position;
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

        [[nodiscard]] exec::task<RenderResult<void>> write(
            std::span<const Write>,
            const std::array<std::uint32_t, 3>& counts
        ) noexcept;
        void finishPreparation(std::uint32_t, RenderResult<void>) noexcept;
        void publish(std::uint32_t kind, std::uint32_t frame) noexcept;
        DeviceSession& device_;
        ResourceUploader& uploads_;
        FrameExecution& frames_;
        StorageId id_;
        cxx::StableSlotMap<Entry, DirectLightTag> entries_;
        std::array<LightArray, 4> arrays_;
        bool updating_ = false;
        std::uint64_t revision_ = 1;
    };

} // namespace lux::render::detail
