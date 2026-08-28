#include <lux/engine/task/TaskExecutorFailureInjection.hpp>

#include <atomic>

namespace lux::task::detail
{
    namespace
    {
        std::atomic_uint32_t pending_failures{};

        [[nodiscard]] constexpr std::uint32_t failureBit(ETaskExecutorFailurePoint point) noexcept
        {
            return 1U << static_cast<std::uint32_t>(point);
        }
    }

    void failNextTaskExecutorOperationForTest(ETaskExecutorFailurePoint point) noexcept
    {
        pending_failures.fetch_or(failureBit(point), std::memory_order_release);
    }

    bool consumeTaskExecutorFailureForTest(ETaskExecutorFailurePoint point) noexcept
    {
        const std::uint32_t bit = failureBit(point);
        std::uint32_t observed = pending_failures.load(std::memory_order_acquire);
        while ((observed & bit) != 0U)
        {
            if (pending_failures.compare_exchange_weak(
                    observed,
                    observed & ~bit,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                ))
            {
                return true;
            }
        }
        return false;
    }
}
