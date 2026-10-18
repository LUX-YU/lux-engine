#pragma once

#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <memory>
#include <vector>

namespace lux::render
{
    inline constexpr auto kInvalidDataReference = error::errorId("lux.render.data.invalid_reference");
    [[nodiscard]] std::span<const error::ErrorDescriptor> renderDataErrorDescriptors() noexcept;

    // Zero-copy asset retention. Charge the complete original capacity, even when
    // only a small range is exposed. There is no second reference-counted owner.
    class RenderPayload final
    {
    public:
        RenderPayload() noexcept = default;
        RenderPayload(const RenderPayload&) noexcept = default;
        RenderPayload& operator=(const RenderPayload& other) noexcept;
        RenderPayload(RenderPayload&& other) noexcept;
        RenderPayload& operator=(RenderPayload&& other) noexcept;

        // Host keeps the Asset module loaded through the final byte Owner destruction.
        [[nodiscard]] static RenderResult<RenderPayload> fromVector(
            std::shared_ptr<const std::vector<std::byte>> owner,
            std::size_t offset,
            std::size_t size
        ) noexcept;

        // Explicit cold copy for author configuration, never an implicit discount
        // on an arbitrary SharedBytes owner's hidden backing.
        [[nodiscard]] static RenderPayload copyOf(std::span<const std::byte> bytes) noexcept;

        // This is a borrow; copy the byte Owner or retain this Payload across calls.
        [[nodiscard]] const lux::cxx::SharedBytes<>& bytes() const noexcept
        {
            return bytes_;
        }

        [[nodiscard]] std::size_t retainedBytes() const noexcept
        {
            return retained_bytes_;
        }

    private:
        lux::cxx::SharedBytes<> bytes_;
        std::size_t retained_bytes_ = 0;
    };
} // namespace lux::render
