#include <lux/engine/render/data/Payload.hpp>
#include <lux/engine/render/data/Schema.hpp>
#include <utility>

namespace lux::render
{
    RenderPayload& RenderPayload::operator=(const RenderPayload& other) noexcept
    {
        if (this != &other)
        {
            bytes_ = other.bytes_;
            retained_bytes_ = other.retained_bytes_;
        }
        return *this;
    }

    RenderPayload::RenderPayload(RenderPayload&& other) noexcept
        : bytes_(std::move(other.bytes_)), retained_bytes_(std::exchange(other.retained_bytes_, 0))
    {
        other.bytes_ = {};
    }

    RenderPayload& RenderPayload::operator=(RenderPayload&& other) noexcept
    {
        if (this != &other)
        {
            bytes_ = std::move(other.bytes_);
            retained_bytes_ = std::exchange(other.retained_bytes_, 0);
            other.bytes_ = {};
        }
        return *this;
    }

    RenderResult<RenderPayload> RenderPayload::fromVector(
        std::shared_ptr<const std::vector<std::byte>> owner,
        std::size_t offset,
        std::size_t size
    ) noexcept
    {
        const bool is_invalid_range = !owner || !size || offset > owner->size() || size > owner->size() - offset;
        if (is_invalid_range)
        {
            return cxx::unexpected(RenderError{kInvalidDataReference});
        }
        RenderPayload result;
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
} // namespace lux::render
