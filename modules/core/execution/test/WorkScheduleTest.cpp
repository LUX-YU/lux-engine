#include <lux/engine/core/execution/WorkSchedule.hpp>
#include <cstdlib>

namespace
{
    enum class EFailure { REJECTED };

    struct Result final
    {
        unsigned worked = 0;
        unsigned completed = 0;
    };

    struct Receiver final
    {
        using receiver_concept = stdexec::receiver_t;
        Result* result;

        void set_value() && noexcept { ++result->completed; }
        void set_error(EFailure) && noexcept { std::abort(); }
        void set_stopped() && noexcept { std::abort(); }
        [[nodiscard]] stdexec::env<> get_env() const noexcept { return {}; }
    };

    constexpr auto mapError = [](auto) noexcept { return EFailure::REJECTED; };
}

int main()
{
    // A non-Render consumer verifies that the common adapter contains no Render result or error contract.
    stdexec::inline_scheduler scheduler;
    auto cpu = lux::execution::CpuSchedule<EFailure>::bind<mapError>(scheduler);
    Result result;
    auto sender = cpu.schedule([](void* value) noexcept { ++static_cast<Result*>(value)->worked; }, &result);
    auto operation = stdexec::connect(std::move(sender), Receiver{&result});
    stdexec::start(operation);
    if (result.worked != 1 || result.completed != 1) std::abort();

    lux::execution::CompletionPost<EFailure> post{
        &result,
        [](void* owner, void* work, lux::execution::CpuResume<EFailure> resume,
            lux::cxx::expected<void, EFailure> outcome) noexcept
        {
            if (owner != work || !outcome) std::abort();
            resume(work, std::move(outcome));
        }
    };
    post(&result, [](void* work, lux::cxx::expected<void, EFailure> outcome) noexcept
    {
        if (!outcome) std::abort();
        ++static_cast<Result*>(work)->completed;
    }, {});
    if (result.completed != 2) std::abort();
}
