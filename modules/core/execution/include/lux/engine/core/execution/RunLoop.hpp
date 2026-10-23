#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <lux/engine/core/execution/visibility.h>
#include <memory>
#include <moodycamel/concurrentqueue.h>
#include <stdexec/execution.hpp>
#include <utility>

namespace lux::execution
{
    namespace detail
    {
        // Execution libraries do not know when a dynamically loaded completion callback returns.
        // A driving loop borrows this environment value before entering that callback's module.
        struct GetCompletionCode final : stdexec::forwarding_query_t
        {
            template <class Env> [[nodiscard]] std::shared_ptr<const void> operator()(const Env& env) const noexcept
            {
                if constexpr (requires { env.query(*this); })
                {
                    return env.query(*this);
                }
                else
                {
                    return {};
                }
            }
        };

        inline constexpr GetCompletionCode getCompletionCode{};
    } // namespace detail

    // A scheduler driven by one existing thread. Producers schedule through stdexec; the driver
    // interleaves ready continuations with its other events. Join producers before destroying it.
    class LUX_CORE_EXECUTION_PUBLIC RunLoop final
    {
    public:
        class Scheduler;
        class ScheduleSender;

        explicit RunLoop(void (*wakeup)(void*) noexcept = nullptr, void* context = nullptr) noexcept;
        ~RunLoop();

        RunLoop(const RunLoop&) = delete;
        RunLoop& operator=(const RunLoop&) = delete;

        [[nodiscard]] Scheduler scheduler() noexcept;

        // Single consumer only. A batch limit yields to other events, not to a frame boundary.
        // Callbacks may schedule more work, but must not recursively drive this loop.
        std::size_t executeReady(std::size_t limit) noexcept;

        // Includes publication in progress and callbacks executing in executeReady(). A nonzero
        // count does not promise a currently dequeueable item; the producer will wake the driver.
        [[nodiscard]] std::size_t size() const noexcept;

        [[nodiscard]] bool empty() const noexcept
        {
            return size() == 0;
        }

        // Empty/joined work does not imply that a producer has returned from the wakeup callback.
        [[nodiscard]] std::size_t activePublishers() const noexcept
        {
            return static_cast<std::uint32_t>(publication_state_.load(std::memory_order_acquire));
        }

        // Observe before consuming/checking completion. Return changes the serial
        // and drops the active count in ONE atomic update, after the wake callback has returned.
        [[nodiscard]] std::uint64_t publicationEpoch() const noexcept
        {
            return publication_state_.load(std::memory_order_acquire);
        }

        // Check publishers before the epoch: the last wake can return after its node was consumed.
        // Pair this with the driver's own notification epoch before entering its blocking wait.
        [[nodiscard]] bool canWait(std::uint64_t observed) const noexcept
        {
            return empty() && activePublishers() == 0 && publicationEpoch() == observed;
        }

    private:
        // Embedded in a connected schedule operation; the queue never owns that operation.
        struct Work
        {
            Work(void (*execute)(Work&) noexcept, std::shared_ptr<const void> code) noexcept
                : execute(execute), code_lifetime(std::move(code))
            {
            }

            Work(const Work&) = delete;
            Work& operator=(const Work&) = delete;

            void (*execute)(Work&) noexcept;
            std::shared_ptr<const void> code_lifetime;
        };

        void post(Work& work) noexcept;

        moodycamel::ConcurrentQueue<Work*> ready_{256};
        moodycamel::ConsumerToken consumer_{ready_};
        std::atomic<std::size_t> pending_{0};
        void (*wakeup_)(void*) noexcept;
        void* wakeup_context_;
        // Low 32 bits: concurrently executing post calls. High 32 bits: returned publications.
        // No loop/context access may follow the final update in post().
        std::atomic<std::uint64_t> publication_state_{0};
    };

    class RunLoop::Scheduler final
    {
    public:
        using scheduler_concept = stdexec::scheduler_t;

        [[nodiscard]] ScheduleSender schedule() const noexcept;
        [[nodiscard]] bool operator==(const Scheduler&) const noexcept = default;

        [[nodiscard]] Scheduler query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>) const noexcept
        {
            return *this;
        }

        [[nodiscard]] stdexec::default_domain query(stdexec::get_completion_domain_t<stdexec::set_value_t>)
            const noexcept
        {
            return {};
        }

        [[nodiscard]] stdexec::forward_progress_guarantee query(stdexec::get_forward_progress_guarantee_t
        ) const noexcept
        {
            return stdexec::forward_progress_guarantee::weakly_parallel;
        }

    private:
        friend class RunLoop;

        explicit Scheduler(RunLoop& loop) noexcept : loop_(&loop) {}

        RunLoop* loop_;
    };

    class RunLoop::ScheduleSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        // This is a guaranteed handoff, including an already determined failure or stopped result.
        // Cancellation belongs to the work itself; it must not swallow a result during delivery.
        using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;

        [[nodiscard]] Scheduler get_env() const noexcept
        {
            return loop_->scheduler();
        }

        template <class Receiver> class Operation final : private Work
        {
        public:
            using operation_state_concept = stdexec::operation_state_t;

            Operation(RunLoop& loop, Receiver receiver) noexcept
                : Work(
                      [](Work& work) noexcept
                      {
                          auto& self = static_cast<Operation&>(work);
                          stdexec::set_value(std::move(self.receiver_));
                          // Terminal delivery may destroy self. No further access.
                      },
                      detail::getCompletionCode(stdexec::get_env(receiver))
                  ),
                  loop_(loop), receiver_(std::move(receiver))
            {
            }

            void start() & noexcept
            {
                loop_.post(*this);
            }

        private:
            RunLoop& loop_;
            Receiver receiver_;
        };

        template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
        {
            return Operation<Receiver>{*loop_, std::move(receiver)};
        }

    private:
        friend class Scheduler;

        explicit ScheduleSender(RunLoop& loop) noexcept : loop_(&loop) {}

        RunLoop* loop_;
    };

    inline RunLoop::Scheduler RunLoop::scheduler() noexcept
    {
        return Scheduler{*this};
    }

    inline RunLoop::ScheduleSender RunLoop::Scheduler::schedule() const noexcept
    {
        return ScheduleSender{*loop_};
    }

    static_assert(stdexec::scheduler<RunLoop::Scheduler>);
} // namespace lux::execution
