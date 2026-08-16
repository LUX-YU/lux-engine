#pragma once
/**
 * @file detail/MainThreadScheduler.hpp
 * @brief Conforming stdexec scheduler over the main completion queue.
 *
 * **Opt-in stdexec header** -- it includes `<stdexec/execution.hpp>`, so any
 * translation unit that includes this one is "infected" by the stdexec headers
 * plus the MSVC `/permissive- /Zc:__cplusplus /Zc:preprocessor` flags they
 * require. The `AsyncRuntime.hpp` facade does not include this file and stays
 * stdexec-free; only explicit sender adapters and this module's own .cpp opt
 * into it.
 *
 * Kept in `namespace lux::exec` (deliberately not anonymous) so the lightweight
 * AsyncRuntime facade can forward-declare it and the opt-in sender adapter
 * can use the same complete type without exposing stdexec to ordinary hosts.
 */

#include <lux/engine/runtime/execution/detail/MainThreadMailbox.hpp>
#include <stdexec/execution.hpp>

#include <utility>

namespace lux::exec
{
    namespace ex = ::stdexec;

    // -- Conforming stdexec scheduler over MainThreadMailbox. Lets
    //    `continues_on(MainThreadScheduler{q})` land the downstream continuation
    //    at the main thread's pump point (drainMainThreadCompletions).
    class MainThreadScheduler
    {
    public:
        explicit MainThreadScheduler(MainThreadMailbox& q) noexcept : q_(&q) {}
        bool operator==(const MainThreadScheduler&) const noexcept = default;

        struct _sender
        {
            using sender_concept = ex::sender_t;
            using completion_signatures =
                ex::completion_signatures<ex::set_value_t(), ex::set_stopped_t()>;

            MainThreadMailbox* q_{nullptr};

            struct _env
            {
                MainThreadMailbox* q_{nullptr};
                MainThreadScheduler query(
                    ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept
                {
                    return MainThreadScheduler{*q_};
                }
            };
            _env get_env() const noexcept { return _env{q_}; }

            template <class Rcvr>
            struct _op
            {
                using operation_state_concept = ex::operation_state_t;
                MainThreadMailbox* q_;
                Rcvr       rcvr_;
                void start() & noexcept
                {
                    q_->enqueue([this]() { ex::set_value(std::move(rcvr_)); });
                }
            };

            template <class Rcvr>
            _op<std::decay_t<Rcvr>> connect(Rcvr&& r) const
            {
                return _op<std::decay_t<Rcvr>>{q_, std::forward<Rcvr>(r)};
            }
        };

        [[nodiscard]] _sender schedule() const noexcept { return _sender{q_}; }

    private:
        MainThreadMailbox* q_{nullptr};
    };

} // namespace lux::exec
