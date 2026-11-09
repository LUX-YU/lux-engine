#pragma once

#include <lux/engine/render/visibility.h>

#include <NRI.h>
#include <array>
#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/extensions/graph/Sampler.hpp>
#include <vector>

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
        [[nodiscard]] std::uint64_t bindingRevision() const noexcept { return binding_revision_; }
        // Recorded commands may retain this Set only while observing bindingRevision on reuse.
        [[nodiscard]] nri::DescriptorSet* descriptors(nri::DescriptorType) const noexcept;
        [[nodiscard]] std::uint32_t capacity(nri::DescriptorType) const noexcept;

    private:
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
            nri::DescriptorPool* pool = nullptr;
            nri::DescriptorSet* descriptors = nullptr;
        };

        struct Sampler final
        {
            nri::SamplerDesc description;
            nri::Descriptor* descriptor;
        };

        [[nodiscard]] RenderResult<void> reserve(nri::DescriptorType, std::uint32_t required) noexcept;

        DeviceSession& session_;
        std::array<Table, 3> tables_;
        std::vector<Sampler> samplers_;
        std::uint64_t allocated_descriptors_ = 0;
        std::uint64_t binding_revision_ = 1;
        std::uint32_t pool_capacity_ = 0;
    };
} // namespace lux::render::detail
