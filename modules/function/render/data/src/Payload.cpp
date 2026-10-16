#include <lux/engine/render/data/Payload.hpp>
#include <utility>
#include <lux/engine/render/data/Schema.hpp>

namespace lux::render
{
    RenderPayload& RenderPayload::operator=(const RenderPayload& other) noexcept
    {
        if (this != &other)
        {
            bytes_ = {};
            source_code_pin_ = other.source_code_pin_;
            bytes_ = other.bytes_;
            retained_bytes_ = other.retained_bytes_;
        }
        return *this;
    }

    RenderPayload::RenderPayload(RenderPayload&& other) noexcept
        : source_code_pin_(std::move(other.source_code_pin_)),
          bytes_(std::move(other.bytes_)),
          retained_bytes_(std::exchange(other.retained_bytes_, 0))
    {
        other.bytes_ = {};
    }

    RenderPayload& RenderPayload::operator=(RenderPayload&& other) noexcept
    {
        if (this != &other)
        {
            bytes_ = {};
            source_code_pin_ = std::move(other.source_code_pin_);
            bytes_ = std::move(other.bytes_);
            retained_bytes_ = std::exchange(other.retained_bytes_, 0);
            other.bytes_ = {};
        }
        return *this;
    }

    RenderResult<RenderPayload> RenderPayload::fromVector(
        std::shared_ptr<const std::vector<std::byte>> owner,
        std::size_t offset,
        std::size_t size,
        const std::shared_ptr<const void>& source_code_pin
    ) noexcept
    {
        const bool is_invalid_range = !owner || !size || offset > owner->size() || size > owner->size() - offset;
        if (is_invalid_range) return cxx::unexpected(RenderError{kInvalidDataReference});
        RenderPayload result;
        result.source_code_pin_ = source_code_pin;
        result.retained_bytes_ = owner->capacity();
        result.bytes_ = lux::cxx::SharedBytes<>::fromOwner(owner, std::span(*owner).subspan(offset, size));
        return result;
    }

    RenderPayload RenderPayload::copyOf(std::span<const std::byte> bytes) noexcept
    {
        RenderPayload result;
        result.bytes_ = lux::cxx::SharedBytes<>::copyOf(bytes);
        result.retained_bytes_ = bytes.size();
        return result;
    }
}
