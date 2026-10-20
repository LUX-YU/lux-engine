#pragma once

#include <lux/engine/render/visibility.h>

#include <NRI.h>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <memory>

namespace lux::render::detail
{
    class DeviceSession;
    class DescriptorArray;
    struct DescriptorSlotTag;

    // Session-owned Native allocation, not a business Storage. A slot is returned only after its actual GPU use ends.
    class LUX_RENDER_PUBLIC DescriptorArray final
    {
    public:
        using Slot = cxx::SlotKey<DescriptorSlotTag>;
        void release(Slot slot) noexcept;
        void write(Slot slot, nri::Descriptor* image, nri::Descriptor* sampler) noexcept;
        ~DescriptorArray() noexcept;
        DescriptorArray(const DescriptorArray&) = delete;
        DescriptorArray& operator=(const DescriptorArray&) = delete;
        [[nodiscard]] RenderResult<cxx::SlotKey<DescriptorSlotTag>> allocate() noexcept;

        [[nodiscard]] nri::DescriptorSet* descriptors() const noexcept
        {
            return descriptors_;
        }

        [[nodiscard]] std::uint32_t capacity() const noexcept
        {
            return capacity_;
        }

        [[nodiscard]] std::size_t occupied() const noexcept
        {
            return slots_.size();
        }

    private:
        friend class DeviceSession;

        struct Entry final
        {
        };

        explicit DescriptorArray(DeviceSession& session, std::uint32_t capacity) noexcept
            : session_(session), capacity_(capacity)
        {
        }

        [[nodiscard]] RenderResult<void> initialize() noexcept;
        [[nodiscard]] RenderResult<void> initializeDefault() noexcept;
        DeviceSession& session_;
        std::uint32_t capacity_;
        cxx::SlotMap<Entry, DescriptorSlotTag> slots_;
        nri::PipelineLayout* allocation_layout_ = nullptr;
        nri::DescriptorPool* pool_ = nullptr;
        nri::DescriptorSet* descriptors_ = nullptr;
        nri::Texture* white_image_ = nullptr;
        nri::Descriptor* white_view_ = nullptr;
        nri::Descriptor* white_sampler_ = nullptr;
        Slot white_slot_{};
    };
} // namespace lux::render::detail
