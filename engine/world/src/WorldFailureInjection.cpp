#include <lux/engine/world/detail/WorldFailureInjection.hpp>

#include <atomic>
#include <cstdint>

namespace lux::world::detail
{
    namespace
    {
        std::atomic_uint32_t g_pending_failures{};

        [[nodiscard]] constexpr std::uint32_t failureBit(
            EWorldFailurePoint point
        ) noexcept
        {
            return std::uint32_t{1U} << static_cast<std::uint8_t>(point);
        }
    }

    void failNextWorldOperationForTest(EWorldFailurePoint point) noexcept
    {
        g_pending_failures.fetch_or(
            failureBit(point),
            std::memory_order_release
        );
    }

    bool consumeWorldFailureForTest(EWorldFailurePoint point) noexcept
    {
        const std::uint32_t bit = failureBit(point);
        std::uint32_t pending = g_pending_failures.load(
            std::memory_order_acquire
        );
        while ((pending & bit) != 0U)
        {
            if (g_pending_failures.compare_exchange_weak(
                    pending,
                    pending & ~bit,
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
