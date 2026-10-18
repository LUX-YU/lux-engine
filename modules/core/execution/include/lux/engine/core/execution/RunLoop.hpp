#pragma once

#include <lux/engine/core/execution/visibility.h>
#include <stdexec/execution.hpp>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>

namespace lux::execution
{
    // A borrowed scheduling destination driven by one owner thread. Producers supply stable nodes;
    // neither posting nor draining allocates. The host joins producers before destroying the loop.
    class LUX_CORE_EXECUTION_PUBLIC RunLoop final
    {
    public:
        class Work
        {
        public:
            explicit Work(void (*execute)(Work&) noexcept) noexcept : execute_(execute) {}
            Work(const Work&) = delete;
            Work& operator=(const Work&) = delete;

        private:
            friend class RunLoop;
            Work* next_ = nullptr;
            void (*execute_)(Work&) noexcept;
        };

        class Scheduler;
        class ScheduleSender;

        explicit RunLoop(void (*wakeup)(void*) noexcept = nullptr, void* context = nullptr) noexcept;
        ~RunLoop();

        RunLoop(const RunLoop&) = delete;
        RunLoop& operator=(const RunLoop&) = delete;

        [[nodiscard]] Scheduler scheduler() noexcept;

        // A node may be reposted only after it has been removed for execution. Publication is the
        // producer's last access to the node: its callback may immediately destroy it on the owner.
        void post(Work& work) noexcept;
        std::size_t executeReady(std::size_t budget) noexcept;
        [[nodiscard]] std::size_t size() const noexcept;
        [[nodiscard]] bool empty() const noexcept { return size() == 0; }

        // Empty/joined work does not imply that a producer has returned from the wakeup callback.
        [[nodiscard]] std::size_t activePublishers() const noexcept
        {
            return active_publishers_.load(std::memory_order_acquire);
        }

    private:
        mutable std::mutex mutex_;
        Work* head_ = nullptr;
        Work* tail_ = nullptr;
        std::size_t size_ = 0;
        void (*wakeup_)(void*) noexcept;
        void* wakeup_context_;
        std::atomic<std::size_t> active_publishers_{0};
    };

    class RunLoop::Scheduler final
    {
    public:
        [[nodiscard]] ScheduleSender schedule() const noexcept;
        [[nodiscard]] bool operator==(const Scheduler&) const noexcept = default;

        [[nodiscard]] stdexec::forward_progress_guarantee
        query(stdexec::get_forward_progress_guarantee_t) const noexcept
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

        [[nodiscard]] auto get_env() const noexcept
        {
            return stdexec::env{
                stdexec::prop{stdexec::get_completion_scheduler<stdexec::set_value_t>, loop_->scheduler()}
            };
        }

        template <class Receiver> class Operation final : private Work
        {
        public:
            using operation_state_concept = stdexec::operation_state_t;

            Operation(RunLoop& loop, Receiver receiver) noexcept
                : Work([](Work& work) noexcept {
                      auto& self = static_cast<Operation&>(work);
                      stdexec::set_value(std::move(self.receiver_));
                      // Terminal delivery may destroy self. No further access.
                  }),
                  loop_(loop), receiver_(std::move(receiver))
            {}

            void start() & noexcept { loop_.post(*this); }

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

    inline RunLoop::Scheduler RunLoop::scheduler() noexcept { return Scheduler{*this}; }

    inline RunLoop::ScheduleSender RunLoop::Scheduler::schedule() const noexcept
    {
        return ScheduleSender{*loop_};
    }

    static_assert(stdexec::scheduler<RunLoop::Scheduler>);
} // namespace lux::execution
