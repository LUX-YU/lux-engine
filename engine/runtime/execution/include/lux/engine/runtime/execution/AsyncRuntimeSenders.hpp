#pragma once
/**
 * @file AsyncRuntimeSenders.hpp
 * @brief Opt-in stdexec adapters for AsyncRuntime.
 */

#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/detail/MainThreadScheduler.hpp>

#include <exec/static_thread_pool.hpp>
#include <exec/tbb/tbb_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace lux::exec
{
    namespace detail
    {
        template <class PublicScheduler, class UnderlyingScheduler>
        class TrackedPoolSender final
        {
        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(),
                stdexec::set_stopped_t()>;

            TrackedPoolSender(
                UnderlyingScheduler scheduler,
                std::shared_ptr<std::atomic<std::size_t>> running) noexcept
                : scheduler_(std::move(scheduler))
                , running_(std::move(running))
            {}

            struct Environment final
            {
                UnderlyingScheduler scheduler;
                std::shared_ptr<std::atomic<std::size_t>> running;

                PublicScheduler query(
                    stdexec::get_completion_scheduler_t<
                        stdexec::set_value_t>) const noexcept
                {
                    return PublicScheduler{scheduler, running};
                }
            };

            [[nodiscard]] Environment get_env() const noexcept
            {
                return {scheduler_, running_};
            }

            template <class Receiver>
            struct TrackedReceiver final
            {
                using receiver_concept = stdexec::receiver_t;

                std::shared_ptr<std::atomic<std::size_t>> running;
                Receiver receiver;

                [[nodiscard]] decltype(auto) get_env() const noexcept
                {
                    return stdexec::get_env(receiver);
                }

                void set_value() && noexcept
                {
                    struct Activity final
                    {
                        std::shared_ptr<std::atomic<std::size_t>> running;
                        explicit Activity(
                            std::shared_ptr<std::atomic<std::size_t>> value)
                            noexcept
                            : running(std::move(value))
                        {
                            running->fetch_add(
                                1u,
                                std::memory_order_relaxed);
                        }
                        ~Activity()
                        {
                            running->fetch_sub(
                                1u,
                                std::memory_order_release);
                        }
                    } activity{running};
                    stdexec::set_value(std::move(receiver));
                }

                void set_stopped() && noexcept
                {
                    stdexec::set_stopped(std::move(receiver));
                }
            };

            template <class Receiver>
            struct Operation final
            {
                using operation_state_concept = stdexec::operation_state_t;
                using ReceiverType = std::decay_t<Receiver>;
                using Upstream = decltype(stdexec::schedule(
                    std::declval<UnderlyingScheduler>()));
                using Inner = stdexec::connect_result_t<
                    Upstream,
                    TrackedReceiver<ReceiverType>>;

                Inner inner;

                void start() & noexcept
                {
                    stdexec::start(inner);
                }
            };

            template <class Receiver>
            [[nodiscard]] Operation<Receiver> connect(
                Receiver&& receiver) const
            {
                auto upstream = stdexec::schedule(scheduler_);
                return {stdexec::connect(
                    std::move(upstream),
                    TrackedReceiver<std::decay_t<Receiver>>{
                        running_,
                        std::forward<Receiver>(receiver)})};
            }

        private:
            UnderlyingScheduler scheduler_;
            std::shared_ptr<std::atomic<std::size_t>> running_;
        };
    }

    class BlockingIoScheduler final
    {
    public:
        using Underlying = decltype(
            std::declval<::experimental::execution::static_thread_pool&>()
                .get_scheduler());
        using Sender = detail::TrackedPoolSender<
            BlockingIoScheduler,
            Underlying>;

        explicit BlockingIoScheduler(AsyncRuntime& runtime) noexcept
            : scheduler_(runtime.blockingIoPool().get_scheduler())
            , running_(runtime.blockingIoActivityCounter())
        {}
        BlockingIoScheduler(
            Underlying scheduler,
            std::shared_ptr<std::atomic<std::size_t>> running) noexcept
            : scheduler_(std::move(scheduler))
            , running_(std::move(running))
        {}

        [[nodiscard]] Sender schedule() const noexcept
        {
            return {scheduler_, running_};
        }

        [[nodiscard]] bool operator==(
            const BlockingIoScheduler& other) const noexcept
        {
            return scheduler_ == other.scheduler_ && running_ == other.running_;
        }

    private:
        Underlying scheduler_;
        std::shared_ptr<std::atomic<std::size_t>> running_;
    };

    class BackgroundCpuScheduler final
    {
    public:
        using Underlying = decltype(
            std::declval<
                ::experimental::execution::tbb::tbb_thread_pool&>()
                .get_scheduler());
        using Sender = detail::TrackedPoolSender<
            BackgroundCpuScheduler,
            Underlying>;

        explicit BackgroundCpuScheduler(AsyncRuntime& runtime) noexcept
            : scheduler_(runtime.backgroundCpuPool().get_scheduler())
            , running_(runtime.backgroundCpuActivityCounter())
        {}
        BackgroundCpuScheduler(
            Underlying scheduler,
            std::shared_ptr<std::atomic<std::size_t>> running) noexcept
            : scheduler_(std::move(scheduler))
            , running_(std::move(running))
        {}

        [[nodiscard]] Sender schedule() const noexcept
        {
            return {scheduler_, running_};
        }

        [[nodiscard]] bool operator==(
            const BackgroundCpuScheduler& other) const noexcept
        {
            return scheduler_ == other.scheduler_ && running_ == other.running_;
        }

    private:
        Underlying scheduler_;
        std::shared_ptr<std::atomic<std::size_t>> running_;
    };

    class AsyncRuntimeCloseSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(AsyncCloseReport)>;

        AsyncRuntimeCloseSender() noexcept = default;

        template <class Receiver>
        struct State final
        {
            using operation_state_concept = stdexec::operation_state_t;
            AsyncRuntime* runtime{nullptr};
            Receiver receiver;

            void start() & noexcept
            {
                if (runtime == nullptr)
                {
                    stdexec::set_value(
                        std::move(receiver), AsyncCloseReport{});
                    return;
                }
                runtime->subscribeClose(
                    [this](AsyncCloseReport report) mutable noexcept
                    {
                        stdexec::set_value(
                            std::move(receiver), std::move(report));
                    });
            }
        };

        template <class Receiver>
        [[nodiscard]] State<std::decay_t<Receiver>> connect(
            Receiver&& receiver) &&
        {
            return {std::exchange(runtime_, nullptr),
                    std::forward<Receiver>(receiver)};
        }

    private:
        friend class AsyncRuntime;
        explicit AsyncRuntimeCloseSender(AsyncRuntime& runtime) noexcept
            : runtime_(&runtime)
        {}

        AsyncRuntime* runtime_{nullptr};
    };

    class AsyncOperationBundleInstallSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using Result = lux::cxx::expected<AsyncOperationBundleLease, EAsyncOperationBundleInstallError>;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(Result)>;

        AsyncOperationBundleInstallSender(
            AsyncClient client,
            AsyncOperationBundle package) noexcept
            : client_(std::move(client)), package_(std::move(package))
        {}

        template <class Receiver>
        struct State final
        {
            using operation_state_concept = stdexec::operation_state_t;
            AsyncClient client;
            AsyncOperationBundle package;
            Receiver receiver;

            void start() & noexcept
            {
                auto completion = [this](
                    lux::cxx::expected<std::uint64_t, EAsyncOperationBundleInstallError>
                        result) mutable noexcept
                {
                    if (!result)
                    {
                        stdexec::set_value(
                            std::move(receiver),
                            Result{lux::cxx::unexpected(result.error())});
                        return;
                    }
                    stdexec::set_value(
                        std::move(receiver),
                        Result{AsyncOperationBundleLease{client, *result}});
                };
                (void)client.tryInstallOperations(
                    std::move(package), std::move(completion));
            }
        };

        template <class Receiver>
        [[nodiscard]] State<std::decay_t<Receiver>> connect(
            Receiver&& receiver) &&
        {
            return {std::move(client_), std::move(package_),
                    std::forward<Receiver>(receiver)};
        }

    private:
        AsyncClient client_;
        AsyncOperationBundle package_;
    };

    class AsyncOperationBundleCloseSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using Result = lux::cxx::expected<AsyncOperationBundleCloseReport, EAsyncOperationBundleCloseError>;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(Result)>;

        explicit AsyncOperationBundleCloseSender(AsyncOperationBundleLease lease) noexcept
            : lease_(std::move(lease))
        {}

        template <class Receiver>
        struct State final
        {
            using operation_state_concept = stdexec::operation_state_t;
            AsyncOperationBundleLease lease;
            Receiver receiver;

            void start() & noexcept
            {
                auto client = lease.client_;
                const auto bundle_id = lease.release();
                auto completion = [this](Result result) mutable noexcept
                {
                    stdexec::set_value(std::move(receiver), std::move(result));
                };
                (void)client.tryCloseOperations(
                    bundle_id, std::move(completion));
            }
        };

        template <class Receiver>
        [[nodiscard]] State<std::decay_t<Receiver>> connect(
            Receiver&& receiver) &&
        {
            return {std::move(lease_), std::forward<Receiver>(receiver)};
        }

    private:
        AsyncOperationBundleLease lease_;
    };

    template <AsyncOperation Operation>
    class AsyncExecuteSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using Outcome = AsyncOutcome<Operation>;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(Outcome),
            stdexec::set_stopped_t()>;

        AsyncExecuteSender(
            AsyncOperationClient<Operation> client,
            Operation operation,
            AsyncSubmitOptions options) noexcept
            : client_(std::move(client))
            , operation_(std::move(operation))
            , options_(options)
        {}

        template <class Receiver>
        struct State final
        {
            using operation_state_concept = stdexec::operation_state_t;
            AsyncOperationClient<Operation> client;
            Operation operation;
            AsyncSubmitOptions options;
            Receiver receiver;

            static void complete(void* opaque, Outcome&& outcome) noexcept
            {
                auto& self = *static_cast<State*>(opaque);
                stdexec::set_value(
                    std::move(self.receiver), std::move(outcome));
            }

            void start() & noexcept
            {
                (void)client.submit(
                    std::move(operation), this, &State::complete, options);
            }
        };

        template <class Receiver>
        [[nodiscard]] State<std::decay_t<Receiver>> connect(
            Receiver&& receiver) &&
        {
            return {std::move(client_), std::move(operation_), options_,
                    std::forward<Receiver>(receiver)};
        }

    private:
        AsyncOperationClient<Operation> client_;
        Operation operation_;
        AsyncSubmitOptions options_{};
    };

    template <AsyncOperation Operation>
    [[nodiscard]] auto execute(
        AsyncOperationClient<Operation> client,
        Operation operation,
        AsyncSubmitOptions options = {}) noexcept
    {
        return AsyncExecuteSender<Operation>{
            std::move(client), std::move(operation), options};
    }

    class CoordinatorScheduler final
    {
    public:
        explicit CoordinatorScheduler(AsyncClient client) noexcept
            : client_(std::move(client))
        {}
        bool operator==(const CoordinatorScheduler&) const noexcept = default;

        struct Sender final
        {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(),
                stdexec::set_stopped_t()>;
            AsyncClient client;

            template <class Receiver>
            struct Operation final
            {
                using operation_state_concept = stdexec::operation_state_t;
                AsyncClient client;
                Receiver receiver;
                void start() & noexcept
                {
                    if (!client.tryDispatchToMainThreadContinuation(
                            [this]() mutable noexcept
                            {
                                stdexec::set_value(std::move(receiver));
                            }))
                        stdexec::set_stopped(std::move(receiver));
                }
            };

            template <class Receiver>
            [[nodiscard]] Operation<std::decay_t<Receiver>> connect(
                Receiver&& receiver) const
            {
                return {client, std::forward<Receiver>(receiver)};
            }
        };

        [[nodiscard]] Sender schedule() const noexcept { return {client_}; }

    private:
        AsyncClient client_;
    };

    class CoordinatorTimerSender final
    {
    public:
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(),
            stdexec::set_stopped_t()>;

        CoordinatorTimerSender(
            AsyncClient client,
            std::chrono::steady_clock::duration delay) noexcept
            : client_(std::move(client)), delay_(delay)
        {}

        template <class Receiver>
        struct State final
        {
            using operation_state_concept = stdexec::operation_state_t;
            using StopToken = stdexec::stop_token_of_t<
                stdexec::env_of_t<Receiver>>;

            struct Cancel final
            {
                AsyncClient client;
                std::shared_ptr<detail::CoordinatorTimerState> timer;

                void operator()() noexcept
                {
                    client.cancelScheduledTimer(timer);
                }
            };

            using StopCallback = stdexec::stop_callback_for_t<
                StopToken,
                Cancel>;

            AsyncClient client;
            std::chrono::steady_clock::duration delay;
            Receiver receiver;
            std::shared_ptr<detail::CoordinatorTimerState> timer{
                std::make_shared<detail::CoordinatorTimerState>()};
            std::optional<StopCallback> stop_callback;

            void start() & noexcept
            {
                stop_callback.emplace(
                    stdexec::get_stop_token(stdexec::get_env(receiver)),
                    Cancel{client, timer});
                if (!client.tryScheduleAfter(
                        delay,
                        timer,
                        [this](bool stopped) mutable noexcept
                        {
                            stop_callback.reset();
                            if (stopped)
                                stdexec::set_stopped(std::move(receiver));
                            else
                                stdexec::set_value(std::move(receiver));
                        }))
                    stdexec::set_stopped(std::move(receiver));
            }
        };

        template <class Receiver>
        [[nodiscard]] State<std::decay_t<Receiver>> connect(
            Receiver&& receiver) &&
        {
            return {std::move(client_), delay_,
                    std::forward<Receiver>(receiver),
                    std::make_shared<detail::CoordinatorTimerState>(),
                    std::nullopt};
        }

    private:
        AsyncClient client_;
        std::chrono::steady_clock::duration delay_{};
    };

    [[nodiscard]] inline BlockingIoScheduler blockingIoScheduler(
        AsyncRuntime& runtime) noexcept
    {
        return BlockingIoScheduler{runtime};
    }

    [[nodiscard]] inline BackgroundCpuScheduler backgroundCpuScheduler(
        AsyncRuntime& runtime) noexcept
    {
        return BackgroundCpuScheduler{runtime};
    }

    [[nodiscard]] inline CoordinatorScheduler coordinatorScheduler(
        AsyncRuntime& runtime) noexcept
    {
        return CoordinatorScheduler{runtime.client()};
    }

    [[nodiscard]] inline CoordinatorTimerSender scheduleAfter(
        AsyncRuntime& runtime,
        std::chrono::steady_clock::duration delay) noexcept
    {
        return CoordinatorTimerSender{runtime.client(), delay};
    }

    [[nodiscard]] inline MainThreadScheduler mainThreadScheduler(
        AsyncRuntime& runtime) noexcept
    {
        return MainThreadScheduler{runtime.mainThreadMailbox()};
    }
}
