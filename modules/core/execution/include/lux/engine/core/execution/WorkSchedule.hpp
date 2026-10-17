#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <exec/any_sender_of.hpp>
#include <exception>
#include <type_traits>
#include <utility>

namespace lux::execution
{
    template<class Error> using CpuSender = typename exec::any_receiver_ref<stdexec::completion_signatures<
        stdexec::set_value_t(), stdexec::set_error_t(Error), stdexec::set_stopped_t()
    >>::template any_sender<>;

    template<class Error> using CpuResume = void (*)(void*, cxx::expected<void, Error>) noexcept;

    // Borrowed notification into the owner's existing bounded pending collection. Publication is the
    // producer's final access; only the owner-thread event consumer may resume or reclaim the operation.
    template<class Error> struct CompletionPost final
    {
        void* owner = nullptr;
        void (*publish)(void*, void*, CpuResume<Error>, cxx::expected<void, Error>) noexcept = nullptr;

        void operator()(void* continuation, CpuResume<Error> resume, cxx::expected<void, Error> result) const noexcept
        {
            publish(owner, continuation, resume, std::move(result));
        }

        [[nodiscard]] explicit operator bool() const noexcept { return publish != nullptr; }
    };

    // Finite operations borrow the existing owner's bounded continuation landing place.
    // Queries forward through Sender adapters; they allocate neither a scheduler nor a queue.
    struct GetCompletionPost final : stdexec::forwarding_query_t
    {
        template <class Env> [[nodiscard]] auto operator()(const Env& env) const noexcept
        { return env.query(*this); }
    };
    inline constexpr GetCompletionPost getCompletionPost{};

    namespace cpu_detail
    {
        using Work = void (*)(void*) noexcept;

        template<class Error, class Scheduler, auto MapError> class WorkSender final
        {
        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(), stdexec::set_error_t(Error), stdexec::set_stopped_t()
            >;

            WorkSender(Scheduler scheduler, Work work, void* owner) noexcept
                : scheduler_(std::move(scheduler)), work_(work), owner_(owner) {}

            template<class Receiver> class Operation final
            {
                struct Scheduled final
                {
                    using receiver_concept = stdexec::receiver_t;
                    Operation* operation;

                    [[nodiscard]] stdexec::env_of_t<const Receiver&> get_env() const noexcept
                    {
                        return stdexec::get_env(std::as_const(operation->receiver_));
                    }

                    void set_value() && noexcept
                    {
                        const auto work = operation->work_;
                        auto* owner = operation->owner_;
                        auto receiver = std::move(operation->receiver_);
                        work(owner);
                        stdexec::set_value(std::move(receiver));
                        // Host retains code through this callback tail; operation may already be destroyed.
                    }

                    template<class ScheduledError> void set_error(ScheduledError&& error) && noexcept
                    {
                        if constexpr (std::is_same_v<std::remove_cvref_t<ScheduledError>, std::exception_ptr>)
                        {
                            std::terminate();
                        }
                        else
                        {
                            auto receiver = std::move(operation->receiver_);
                            const Error mapped = MapError(std::forward<ScheduledError>(error));
                            stdexec::set_error(std::move(receiver), mapped);
                        }
                    }

                    void set_stopped() && noexcept
                    {
                        auto receiver = std::move(operation->receiver_);
                        stdexec::set_stopped(std::move(receiver));
                    }
                };

                using ScheduledSender = decltype(stdexec::schedule(std::declval<Scheduler>()));
                using ScheduledOperation = stdexec::connect_result_t<ScheduledSender, Scheduled>;

            public:
                using operation_state_concept = stdexec::operation_state_t;

                Operation(WorkSender input, Receiver receiver)
                    : receiver_(std::move(receiver)), work_(input.work_), owner_(input.owner_),
                      scheduled_(stdexec::connect(stdexec::schedule(input.scheduler_), Scheduled{this})) {}

                Operation(const Operation&) = delete;
                Operation& operator=(const Operation&) = delete;
                Operation(Operation&&) = delete;
                Operation& operator=(Operation&&) = delete;

                void start() & noexcept { stdexec::start(scheduled_); }

            private:
                Receiver receiver_;
                Work work_;
                void* owner_;
                ScheduledOperation scheduled_;
            };

            template<class Receiver> [[nodiscard]] Operation<std::decay_t<Receiver>> connect(Receiver&& receiver) &&
            {
                return {std::move(*this), std::forward<Receiver>(receiver)};
            }

        private:
            Scheduler scheduler_;
            Work work_;
            void* owner_;
        };
    }

    // One adapter to a real host scheduler. The borrowed scheduler and this factory's module must
    // outlive all accepted work, including its callback stacks; no thread or queue is created here.
    template<class Error> class CpuSchedule final
    {
    public:
        using Work = cpu_detail::Work;
        CpuSchedule() noexcept = default;

        template<auto MapError, class Scheduler> [[nodiscard]] static CpuSchedule bind(Scheduler& scheduler) noexcept
        {
            return {&scheduler, [](void* host, Work work, void* owner) noexcept -> CpuSender<Error> {
                return cpu_detail::WorkSender<Error, Scheduler, MapError>{*static_cast<Scheduler*>(host), work, owner};
            }};
        }

        [[nodiscard]] CpuSender<Error> schedule(Work work, void* owner) const noexcept
        {
            return schedule_(host_, work, owner);
        }

        [[nodiscard]] explicit operator bool() const noexcept { return schedule_ != nullptr; }

    private:
        using Schedule = CpuSender<Error> (*)(void*, Work, void*) noexcept;
        CpuSchedule(void* host, Schedule schedule) noexcept : host_(host), schedule_(schedule) {}
        void* host_ = nullptr;
        Schedule schedule_ = nullptr;
    };
}
