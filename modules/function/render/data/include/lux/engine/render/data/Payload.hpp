#pragma once

#include <memory>
#include <vector>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/render/core/Error.hpp>

namespace lux::render
{
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

        // A separately loaded Asset module supplies its code pin here: the byte
        // Owner's deleter and control block may belong to a different module than
        // the Command. Without a pin their code must outlive every source Owner,
        // normally by remaining loaded for the lifetime of the process.
        [[nodiscard]] static RenderResult<RenderPayload> fromVector(
            std::shared_ptr<const std::vector<std::byte>> owner,
            std::size_t offset,
            std::size_t size,
            const std::shared_ptr<const void>& source_code_pin = {}
        ) noexcept;

        // Explicit cold copy for author configuration, never an implicit discount
        // on an arbitrary SharedBytes owner's hidden backing.
        [[nodiscard]] static RenderPayload copyOf(std::span<const std::byte> bytes) noexcept;

        // This is a borrow. Retain the complete RenderPayload across calls: a
        // separately copied SharedBytes does not retain the source module pin.
        [[nodiscard]] const lux::cxx::SharedBytes<>& bytes() const noexcept { return bytes_; }
        [[nodiscard]] std::size_t retainedBytes() const noexcept { return retained_bytes_; }

    private:
        // Destroy the byte Owner completely before releasing its deleter's code.
        std::shared_ptr<const void> source_code_pin_;
        lux::cxx::SharedBytes<> bytes_;
        std::size_t retained_bytes_ = 0;
    };
}
