#include <lux/engine/render/extensions/upload/Preparation.hpp>

namespace lux::render::detail
{
    Preparation::Preparation(std::size_t resource_capacity) noexcept
    {
        receivers_.reserve(resource_capacity);
    }

    Preparation::~Preparation() noexcept
    {
        const bool has_pending_delivery = delivering_ || !receivers_.empty();
        if (has_pending_delivery) std::terminate();
    }

    void Preparation::onSubmitted(void* receiver, Submitted complete) noexcept
    {
        if (!complete) std::terminate();
        receivers_.emplace_back(receiver, complete);
    }

    void Preparation::finishCallbacks(std::uint32_t slot, RenderResult<void> result) noexcept
    {
        if (delivering_) std::terminate();
        delivering_ = true;
        const auto boundary = receivers_.size();
        for (std::size_t i = 0; i < boundary; ++i)
        {
            const auto [receiver, complete] = std::exchange(receivers_[i], {});
            complete(receiver, slot, result);
        }
        receivers_.erase(receivers_.begin(), receivers_.begin() + boundary);
        delivering_ = false;
    }
} // namespace lux::render::detail
