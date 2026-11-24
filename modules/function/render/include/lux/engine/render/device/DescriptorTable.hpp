#pragma once

#include <lux/cxx/container/StableSlotMap.hpp>

#include <lux/engine/render/visibility.h>

#include <NRI.h>
#include <array>
#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/graph/Sampler.hpp>
#include <vector>

namespace lux::render::runtime_detail { struct DeviceRound; }

namespace lux::render
{
    struct DeviceOptions;
}

namespace lux::render::detail
{
    class DeviceSession;
    struct DescriptorSlotTag;

    // Device-thread owner of stable descriptor indices and replaceable physical tables.
    // Resource views are borrowed until release at their proven GPU completion boundary.
    class LUX_RENDER_PUBLIC DescriptorTable final
    {
    public:
        using Slot = cxx::SlotKey<DescriptorSlotTag>;

        [[nodiscard]] static std::array<nri::DescriptorRangeDesc, 3>
        layoutRanges(const std::array<std::uint32_t, 3>& capacities, bool variable_count) noexcept;

        explicit DescriptorTable(DeviceSession& session) noexcept : session_(session) {}

        ~DescriptorTable() noexcept;
        DescriptorTable(const DescriptorTable&) = delete;
        DescriptorTable& operator=(const DescriptorTable&) = delete;

        // pool_capacity is the backend's aggregate UAB pool limit, including retiring pools.
        [[nodiscard]] RenderResult<void> initialize(const DeviceOptions&, std::uint32_t pool_capacity) noexcept;
        void clear() noexcept;
        [[nodiscard]] RenderResult<Slot> allocate(nri::DescriptorType) noexcept;
        void release(nri::DescriptorType, Slot) noexcept;
        // Fill a newly issued slot before publishing its resource. Existing contents are immutable.
        void write(nri::DescriptorType, Slot, nri::Descriptor*) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> sampler(const nri::SamplerDesc&) noexcept;
        [[nodiscard]] RenderResult<nri::Descriptor*> sampler(const SamplerDescription&) noexcept;

        // The physical table generation changes only after a replacement Set is published.
        [[nodiscard]] std::uint64_t nativeBindingGeneration() const noexcept { return native_binding_generation_; }
        [[nodiscard]] std::uint64_t nativeSetGeneration(nri::DescriptorType) const noexcept;
        [[nodiscard]] std::uint64_t contentRevision() const noexcept { return content_revision_; }

        // Recorded commands may retain this Set only while observing the native generation on reuse.
        [[nodiscard]] nri::DescriptorSet* descriptors(nri::DescriptorType) const noexcept;
        [[nodiscard]] std::uint32_t capacity(nri::DescriptorType) const noexcept;
        [[nodiscard]] nri::Descriptor* textureView(Slot slot) const noexcept;


        // D3D12 binds one resource/sampler heap pair. Copy shared tables into the writable frame's pool.
        struct Snapshot final
        {
            const DescriptorTable* owner = nullptr;
            std::array<nri::DescriptorSet*, 3> sets{};
            std::array<std::uint32_t, 3> capacities{};
            std::uint64_t generation = 0;
            std::uint64_t revision = 0;
        };
        [[nodiscard]] std::array<std::uint32_t, 3> snapshotCounts() const noexcept;
        [[nodiscard]] RenderResult<void> addSnapshotDescriptors(nri::DescriptorPoolDesc&) const noexcept;
        [[nodiscard]] RenderResult<Snapshot> createSnapshot(nri::DescriptorPool&) const noexcept;
        [[nodiscard]] RenderResult<void> updateSnapshot(Snapshot&) const noexcept;

    private:
        friend struct runtime_detail::DeviceRound;

        struct NativePoolTag;
        using NativePoolId = cxx::SlotKey<NativePoolTag>;

        struct NativePool final
        {
            nri::DescriptorPool* pool;
            std::uint32_t capacity;
            std::uint32_t readers = 0;
            bool replaced = false;
        };

        struct Entry final
        {
            nri::Descriptor* descriptor = nullptr;
        };

        struct Table final
        {
            std::uint32_t layout_capacity = 0;
            std::uint32_t allocated_capacity = 0;
            cxx::SlotKeyAutoSparseSet<Slot, Entry> slots;
            nri::PipelineLayout* allocation_layout = nullptr;
            NativePoolId pool;
            std::uint64_t generation = 0;
            nri::DescriptorSet* descriptors = nullptr;
        };

        struct ChangedSlot final
        {
            Slot slot;
            std::uint32_t kind;
        };

        static constexpr std::size_t change_capacity = 256;
        void markChanged(nri::DescriptorType type, Slot slot) noexcept;
        void copyLiveDescriptors(const Snapshot&) const noexcept;
        void copyChangedDescriptors(const Snapshot&) const noexcept;

        struct Sampler final
        {
            nri::SamplerDesc description;
            nri::Descriptor* descriptor;
        };

        [[nodiscard]] RenderResult<void> reserve(nri::DescriptorType, std::uint32_t required) noexcept;
        void retainFramePools(std::vector<NativePoolId>& held) noexcept;
        void releaseFramePools(std::vector<NativePoolId>& held) noexcept;
        void retirePool(NativePoolId) noexcept;
        void destroyPool(NativePoolId) noexcept;

        DeviceSession& session_;
        std::array<Table, 3> tables_;
        std::vector<Sampler> samplers_;
        std::vector<ChangedSlot> changes_;
        cxx::StableSlotMap<NativePool, NativePoolTag> pools_;
        std::uint64_t allocated_descriptors_ = 0;
        std::uint64_t native_binding_generation_ = 1;
        std::uint64_t content_revision_ = 1;
        std::uint32_t pool_capacity_ = 0;
    };
} // namespace lux::render::detail
