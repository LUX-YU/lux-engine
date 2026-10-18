#include <lux/engine/core/execution/CompletionPost.hpp>
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

}

int main()
{
    // Standard CPU composition and the independent owner-notification contract.
    stdexec::inline_scheduler scheduler;
    Result result;
    auto sender = stdexec::schedule(scheduler) | stdexec::then([&result]() noexcept { ++result.worked; });
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
