#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace lux::scene
{
    template <class Type>
    class LatestSpscExchange final
    {
    public:
        static_assert(std::is_default_constructible_v<Type>);

        LatestSpscExchange() = default;
        LatestSpscExchange(const LatestSpscExchange&) = delete;
        LatestSpscExchange& operator=(const LatestSpscExchange&) = delete;
        LatestSpscExchange(LatestSpscExchange&&) = delete;
        LatestSpscExchange& operator=(LatestSpscExchange&&) = delete;

        [[nodiscard]] Type& write() noexcept
        {
            return slots_[back_index_];
        }

        void publish() noexcept
        {
            const std::uint32_t previous = middle_state_.exchange(
                pack(back_index_, true),
                std::memory_order_acq_rel
            );
            back_index_ = index(previous);
        }

        [[nodiscard]] bool acquireLatest() noexcept
        {
            const std::uint32_t observed = middle_state_.load(std::memory_order_acquire);
            if (!hasNewData(observed))
                return false;
            const std::uint32_t previous = middle_state_.exchange(
                pack(front_index_, false),
                std::memory_order_acq_rel
            );
            front_index_ = index(previous);
            return true;
        }

        [[nodiscard]] const Type& read() const noexcept
        {
            return slots_[front_index_];
        }

    private:
        [[nodiscard]] static constexpr std::uint32_t pack(
            std::uint32_t slot,
            bool has_new
        ) noexcept
        {
            return slot | (has_new ? 0x4U : 0U);
        }

        [[nodiscard]] static constexpr std::uint32_t index(std::uint32_t state) noexcept
        {
            return state & 0x3U;
        }

        [[nodiscard]] static constexpr bool hasNewData(std::uint32_t state) noexcept
        {
            return (state & 0x4U) != 0U;
        }

        std::array<Type, 3U> slots_{};
        std::atomic<std::uint32_t> middle_state_{pack(1U, false)};
        std::uint32_t front_index_{};
        std::uint32_t back_index_{2U};
    };
} // namespace lux::scene
