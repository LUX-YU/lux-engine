#pragma once

#include <lux/engine/render/visibility.h>

#include <NRI.h>
#include <array>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <vector>

namespace lux::render::detail
{
    class DeviceSession;
    struct DescriptorSlotTag;

    // Device-wide descriptor indices. Texture slots retire with their GPU use; immutable samplers live to close.
    class LUX_RENDER_PUBLIC DescriptorTable final
    {
    public:
        using Slot = cxx::SlotKey<DescriptorSlotTag>;
        [[nodiscard]] static std::array<nri::DescriptorRangeDesc, 2> layoutRanges(
            std::uint32_t images,
            std::uint32_t samplers
        ) noexcept;

        explicit DescriptorTable(DeviceSession& session) noexcept : session_(session) {}

        ~DescriptorTable() noexcept;
        DescriptorTable(const DescriptorTable&) = delete;
        DescriptorTable& operator=(const DescriptorTable&) = delete;

        [[nodiscard]] RenderResult<void> initialize(
            std::uint32_t image_capacity,
            std::uint32_t sampler_capacity
        ) noexcept;
        void clear() noexcept;
        [[nodiscard]] RenderResult<Slot> allocate() noexcept;
        void release(Slot) noexcept;
        void write(Slot, nri::Descriptor* image) noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> sampler(const nri::SamplerDesc&) noexcept;

        [[nodiscard]] nri::DescriptorSet* descriptors() const noexcept
        {
            return descriptors_;
        }

        [[nodiscard]] std::uint32_t imageCapacity() const noexcept
        {
            return image_capacity_;
        }

        [[nodiscard]] std::uint32_t samplerCapacity() const noexcept
        {
            return sampler_capacity_;
        }

        [[nodiscard]] std::size_t occupied() const noexcept
        {
            return slots_.size();
        }

        [[nodiscard]] std::size_t samplerCount() const noexcept
        {
            return samplers_.size();
        }

    private:
        struct Entry final
        {
        };

        struct Sampler final
        {
            nri::SamplerDesc description;
            nri::Descriptor* descriptor;
        };

        DeviceSession& session_;
        std::uint32_t image_capacity_ = 0, sampler_capacity_ = 0;
        cxx::SlotMap<Entry, DescriptorSlotTag> slots_;
        std::vector<Sampler> samplers_;
        nri::PipelineLayout* allocation_layout_ = nullptr;
        nri::DescriptorPool* pool_ = nullptr;
        nri::DescriptorSet* descriptors_ = nullptr;
    };
} // namespace lux::render::detail
