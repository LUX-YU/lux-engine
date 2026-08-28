#include <lux/engine/process/detail/TimerFailureInjection.hpp>

#include <atomic>

namespace lux::process::detail::testing
{
    namespace
    {
        std::atomic<ETimerCreateFailure> next_failure{ETimerCreateFailure::NONE};
    }

    void failNextTimerCreate(ETimerCreateFailure failure) noexcept
    {
        next_failure.store(failure, std::memory_order_release);
    }

    ETimerCreateFailure consumeTimerCreateFailure() noexcept
    {
        return next_failure.exchange(ETimerCreateFailure::NONE, std::memory_order_acq_rel);
    }
}
