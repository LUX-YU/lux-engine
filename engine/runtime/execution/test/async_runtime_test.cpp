#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace ex = stdexec;

namespace
{
    enum class ETestError : std::uint8_t
    {
        REJECTED
    };

    struct DoubleValue final
    {
        using Value = int;
        using Error = ETestError;

        int value{0};
        std::shared_ptr<const int> owner;
    };

    struct MissingOperation final
    {
        using Value = int;
        using Error = ETestError;
    };

    struct CloseFromCoordinator final
    {
        using Value = lux::exec::EAsyncCloseStatus;
        using Error = ETestError;
    };

    struct TripleValue final
    {
        using Value = int;
        using Error = ETestError;

        int value{0};
    };

    struct CountOperation final
    {
        using Value = void;
        using Error = ETestError;
    };

    struct CloseRaceOperation final
    {
        using Value = void;
        using Error = ETestError;

        std::uint32_t producer{0u};
        std::uint32_t sequence{0u};
    };

    struct NonDefaultOperation final
    {
        using Value = int;
        using Error = ETestError;

        explicit NonDefaultOperation(int input) noexcept : value(input) {}
        NonDefaultOperation() = delete;

        int value;
    };

    template <class Sender>
    auto wait(Sender&& sender)
    {
        return ex::sync_wait(std::forward<Sender>(sender));
    }
}

int main()
{
    int failures = 0;
    auto check = [&](bool condition, const char* message)
    {
        if (condition)
            std::printf("[ ok ] %s\n", message);
        else
        {
            std::printf("[FAIL] %s\n", message);
            ++failures;
        }
        std::fflush(stdout);
    };

    std::atomic<int> coordinator_count{0};
    lux::exec::AsyncRuntimeBuilder builder;
    auto added = builder.addOperation<DoubleValue>(
        [](DoubleValue&& operation,
           lux::exec::AsyncOperationContext& context,
           auto&& completion) noexcept
        {
            const int input = operation.value +
                (operation.owner ? *operation.owner : 0);
            auto work = ex::just(input)
                | ex::continues_on(
                      lux::exec::backgroundCpuScheduler(context.runtime()))
                | ex::then(
                      [completion = std::move(completion)](
                          int value) mutable noexcept
                      {
                          completion.complete(value * 2);
                      });
            (void)lux::exec::spawn(context.scope(), std::move(work));
            }, {}, lux::exec::AsyncOperationQueueConfig{
                .capacity = 1024,
                .byte_budget = 1024u * 1024u,
                .drain_batch = 64});
    check(added.has_value(), "register typed operation");
    auto double_client = added ? *added
                               : lux::exec::AsyncOperationClient<DoubleValue>{};

    auto duplicate = builder.addOperation<DoubleValue>(
        [](DoubleValue&&,
           lux::exec::AsyncOperationContext&,
           auto&& completion) noexcept
        {
            completion.failRuntime(
                lux::exec::EAsyncSubmitError::PAYLOAD_INVALID);
        });
    check(
        !duplicate && duplicate.error().code ==
            lux::exec::EAsyncAssemblyError::DUPLICATE_OPERATION,
        "duplicate type rejected without RTTI");

    auto count_added = builder.addOperation<CountOperation>(
        [&coordinator_count](CountOperation&&,
           lux::exec::AsyncOperationContext&,
           auto&& completion) noexcept
        {
            coordinator_count.fetch_add(1, std::memory_order_relaxed);
            completion.complete({});
        }, {}, lux::exec::AsyncOperationQueueConfig{
            .capacity = 1024,
            .byte_budget = 1024u * 1024u,
            .drain_batch = 32});
    check(count_added.has_value(), "register MPMC count operation");
    auto count_client = count_added
        ? *count_added
        : lux::exec::AsyncOperationClient<CountOperation>{};

    auto non_default_added = builder.addOperation<NonDefaultOperation>(
        [](NonDefaultOperation&& operation,
           lux::exec::AsyncOperationContext&,
           auto&& completion) noexcept
        {
            completion.complete(operation.value);
        });
    check(
        non_default_added.has_value(),
        "typed queue does not require default-constructible payloads");
    auto non_default_client = non_default_added
        ? *non_default_added
        : lux::exec::AsyncOperationClient<NonDefaultOperation>{};

    auto plan = std::move(builder).compile();
    check(plan.has_value(), "compile operation registry");
    if (!plan)
        return failures + 1;

    lux::exec::AsyncRuntime runtime(
        std::move(*plan),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1,
            .background_cpu_concurrency = 2,
            .enable_latency_histograms = true});

    {
        auto first_wakes = std::make_shared<std::atomic<int>>(0);
        auto second_wakes = std::make_shared<std::atomic<int>>(0);
        const auto make_binding = [](auto owner)
        {
            return std::make_shared<
                const lux::exec::MainThreadMailbox::WakeBinding>(
                lux::exec::MainThreadMailbox::WakeBinding{
                    owner,
                    [](void* opaque) noexcept
                    {
                        static_cast<std::atomic<int>*>(opaque)->fetch_add(
                            1,
                            std::memory_order_relaxed);
                    }});
        };
        auto first_binding = make_binding(first_wakes);
        auto second_binding = make_binding(second_wakes);
        runtime.mainThreadMailbox().bindExternalWake(first_binding);
        (void)runtime.mainThreadDispatcher().tryDispatchToMainThread([]() noexcept {});
        runtime.mainThreadMailbox().bindExternalWake(second_binding);
        runtime.mainThreadMailbox().unbindExternalWake(first_binding);
        (void)runtime.mainThreadDispatcher().tryDispatchToMainThread([]() noexcept {});
        runtime.mainThreadMailbox().unbindExternalWake(second_binding);
        (void)runtime.mainThreadDispatcher().tryDispatchToMainThread([]() noexcept {});
        check(
            first_wakes->load(std::memory_order_relaxed) == 1 &&
                second_wakes->load(std::memory_order_relaxed) == 1,
            "stale wake unbind cannot clear a newer generation");
        (void)runtime.drainMainThreadCompletions();
    }

    auto owned = std::make_shared<const int>(3);
    auto result = wait(lux::exec::execute(
        double_client,
        DoubleValue{4, owned}));
    owned.reset();
    check(
        result && std::get<0>(*result).has_value() &&
            *std::get<0>(*result) == 14,
        "operation owns payload and completes from oneTBB background arena");

    {
        lux::exec::AsyncScope activity_scope{runtime};
        std::atomic<bool> cpu_started{false};
        std::atomic<bool> release_cpu{false};
        auto cpu_work = ex::schedule(
                lux::exec::backgroundCpuScheduler(runtime))
            | ex::then(
                  [&cpu_started, &release_cpu]() noexcept
                  {
                      cpu_started.store(true, std::memory_order_release);
                      cpu_started.notify_one();
                      release_cpu.wait(false, std::memory_order_acquire);
                  });
        check(
            lux::exec::spawn(activity_scope, std::move(cpu_work)),
            "tracked background CPU activity is admitted");
        cpu_started.wait(false, std::memory_order_acquire);
        check(
            runtime.stats().background_cpu_running == 1u,
            "runtime reports a currently executing TBB continuation");
        release_cpu.store(true, std::memory_order_release);
        release_cpu.notify_one();
        lux::exec::testing::closeScope(activity_scope, runtime);
    }

    {
        lux::exec::AsyncScope activity_scope{runtime};
        std::atomic<bool> io_started{false};
        std::atomic<bool> release_io{false};
        auto io_work = ex::schedule(lux::exec::blockingIoScheduler(runtime))
            | ex::then(
                  [&io_started, &release_io]() noexcept
                  {
                      io_started.store(true, std::memory_order_release);
                      io_started.notify_one();
                      release_io.wait(false, std::memory_order_acquire);
                  });
        check(
            lux::exec::spawn(activity_scope, std::move(io_work)),
            "tracked BlockingIO activity is admitted");
        io_started.wait(false, std::memory_order_acquire);
        check(
            runtime.stats().blocking_io_running == 1u,
            "runtime reports a currently executing BlockingIO continuation");
        release_io.store(true, std::memory_order_release);
        release_io.notify_one();
        lux::exec::testing::closeScope(activity_scope, runtime);
    }

    auto non_default_result = wait(lux::exec::execute(
        non_default_client,
        NonDefaultOperation{9}));
    check(
        non_default_result && std::get<0>(*non_default_result).has_value() &&
            *std::get<0>(*non_default_result) == 9,
        "non-default operation survives enqueue/dequeue ownership transfer");

    auto unknown = wait(lux::exec::execute(
        lux::exec::AsyncOperationClient<MissingOperation>{},
        MissingOperation{}));
    check(
        unknown && !std::get<0>(*unknown) &&
            std::get<0>(*unknown).error().isRuntime() &&
            std::get<0>(*unknown).error().runtimeError() ==
                lux::exec::EAsyncSubmitError::UNKNOWN_OPERATION,
        "unknown operation is a structured value failure");

    std::weak_ptr<int> dynamic_module;
    {
        auto module = std::make_shared<int>(42);
        dynamic_module = module;
        lux::exec::AsyncRuntimeBuilder bundle_builder;
        lux::exec::AsyncOperationRegistrationOptions options;
        options.module_lease = module;
        auto bundle_added = bundle_builder.addOperation<TripleValue>(
            [](TripleValue&& operation,
               lux::exec::AsyncOperationContext& context,
               auto&& completion) noexcept
            {
                auto work = ex::just(operation.value)
                    | ex::continues_on(
                          lux::exec::backgroundCpuScheduler(context.runtime()))
                    | ex::then(
                          [completion = std::move(completion)](
                              int value) mutable noexcept
                          {
                              completion.complete(value * 3);
                          });
                (void)lux::exec::spawn(context.scope(), std::move(work));
            },
            std::move(options),
            lux::exec::AsyncOperationQueueConfig{
                .capacity = 64,
                .byte_budget = 1024u * 1024u,
                .drain_batch = 16});
        check(bundle_added.has_value(), "assemble dynamic operation bundle package");
        auto triple_client = bundle_added
            ? *bundle_added
            : lux::exec::AsyncOperationClient<TripleValue>{};

        auto installed_wait = wait(runtime.installOperations(
            std::move(bundle_builder).compileOperations()));
        check(
            installed_wait && std::get<0>(*installed_wait).has_value(),
            "install dynamic operation bundle on coordinator");
        if (installed_wait && std::get<0>(*installed_wait))
        {
            auto bundle_lease = std::move(*std::get<0>(*installed_wait));
            module.reset();
            check(
                !dynamic_module.expired(),
                "installed registry retains dynamic module lease");

            auto dynamic_result = wait(lux::exec::execute(
                triple_client,
                TripleValue{7}));
            check(
                dynamic_result &&
                    std::get<0>(*dynamic_result).has_value() &&
                    *std::get<0>(*dynamic_result) == 21,
                "execute dynamically installed typed operation");

            auto closed = lux::exec::testing::closeOperations(
                runtime,
                std::move(bundle_lease));
            check(
                closed && closed->removed_operations == 1u,
                "operation bundle close waits for scope and removes registrations");
            check(
                dynamic_module.expired(),
                "module lease releases after operation bundle scope is empty");

            auto removed_result = wait(lux::exec::execute(
                triple_client,
                TripleValue{1}));
            check(
                removed_result && !std::get<0>(*removed_result) &&
                    std::get<0>(*removed_result).error().isRuntime() &&
                    std::get<0>(*removed_result).error().runtimeError() ==
                        lux::exec::EAsyncSubmitError::UNKNOWN_OPERATION,
                "closed operation bundle rejects later operations structurally");
        }
    }

    lux::exec::AsyncRuntimeBuilder missing_bundle_builder;
    lux::exec::AsyncOperationRegistrationOptions bundle_dependency;
    bundle_dependency.prerequisites.push_back(
        lux::exec::kAsyncTypeToken<MissingOperation>);
    (void)missing_bundle_builder.addOperation<TripleValue>(
        [](TripleValue&&,
           lux::exec::AsyncOperationContext&,
           auto&& completion) noexcept
        {
            completion.failRuntime(
                lux::exec::EAsyncSubmitError::PAYLOAD_INVALID);
        },
        std::move(bundle_dependency));
    auto missing_bundle_wait = wait(runtime.installOperations(
        std::move(missing_bundle_builder).compileOperations()));
    check(
        missing_bundle_wait && !std::get<0>(*missing_bundle_wait) &&
            std::get<0>(*missing_bundle_wait).error() ==
                lux::exec::EAsyncOperationBundleInstallError::MISSING_DEPENDENCY,
        "dynamic install validates dependencies against live registry");

    {
        constexpr std::uint32_t kProducerCount = 16u;
        constexpr std::uint32_t kSubmitsPerProducer = 6250u;
        auto module = std::make_shared<int>(91);
        std::weak_ptr<int> module_lifetime = module;
        std::atomic<std::size_t> handled{0u};
        std::atomic<bool> start{false};

        lux::exec::AsyncRuntimeBuilder race_builder;
        lux::exec::AsyncOperationRegistrationOptions race_options;
        race_options.module_lease = module;
        auto race_added = race_builder.addOperation<CloseRaceOperation>(
            [&handled](CloseRaceOperation&&,
                       lux::exec::AsyncOperationContext&,
                       auto&& completion) noexcept
            {
                handled.fetch_add(1u, std::memory_order_relaxed);
                completion.complete({});
            },
            std::move(race_options),
            lux::exec::AsyncOperationQueueConfig{
                .capacity = 4096u,
                .byte_budget = 4u * 1024u * 1024u,
                .drain_batch = 64u});
        check(race_added.has_value(), "assemble endpoint close race operation bundle");
        auto race_client = race_added
            ? *race_added
            : lux::exec::AsyncOperationClient<CloseRaceOperation>{};
        auto installed_race = wait(runtime.installOperations(
            std::move(race_builder).compileOperations()));
        check(
            installed_race && std::get<0>(*installed_race).has_value(),
            "install endpoint close race operation bundle");

        if (installed_race && std::get<0>(*installed_race))
        {
            auto race_lease = std::move(*std::get<0>(*installed_race));
            module.reset();
            std::vector<std::thread> race_producers;
            race_producers.reserve(kProducerCount);
            for (std::uint32_t producer = 0u;
                 producer < kProducerCount;
                 ++producer)
            {
                race_producers.emplace_back(
                    [race_client, &start, producer]() mutable noexcept
                    {
                        start.wait(false, std::memory_order_acquire);
                        for (std::uint32_t sequence = 0u;
                             sequence < kSubmitsPerProducer;
                             ++sequence)
                        {
                            (void)race_client.tryNotify(
                                CloseRaceOperation{producer, sequence},
                                lux::exec::AsyncSubmitOptions{
                                    .accounted_bytes =
                                        sizeof(CloseRaceOperation)});
                        }
                    });
            }
            start.store(true, std::memory_order_release);
            start.notify_all();

            auto closed_race = lux::exec::testing::closeOperations(
                runtime,
                std::move(race_lease));
            for (auto& producer : race_producers)
                producer.join();

            check(
                closed_race && closed_race->removed_operations == 1u,
                "operation bundle close linearizes against 100k concurrent submits");
            check(
                module_lifetime.expired(),
                "module lease outlives every admitted producer and queued item");
        }
    }

    std::vector<std::thread> producers;
    producers.reserve(16);
    for (int producer = 0; producer < 16; ++producer)
    {
        producers.emplace_back(
            [client = count_client]() mutable
            {
                for (int i = 0; i < 32; ++i)
                {
                    for (;;)
                    {
                        auto accepted = client.tryNotify(
                            CountOperation{});
                        if (accepted)
                            break;
                        std::this_thread::yield();
                    }
                }
            });
    }
    for (auto& producer : producers)
        producer.join();
    while (coordinator_count.load(std::memory_order_acquire) != 512)
        std::this_thread::yield();
    check(
        coordinator_count.load(std::memory_order_relaxed) == 512,
        "16 producers submit through one sleeping MPMC ingress");

    std::atomic<bool> close_requested_from_main{false};
    std::atomic<bool> close_completed_from_main{false};
    auto close_from_main = lux::exec::EAsyncCloseStatus::CLOSE_IN_PROGRESS;
    check(
        runtime.mainThreadDispatcher().tryDispatchToMainThread(
            [&runtime,
             &close_from_main,
             &close_requested_from_main,
             &close_completed_from_main]() noexcept
            {
                lux::exec::detail::subscribeRuntimeClose(
                    runtime,
                    [&close_from_main, &close_completed_from_main](
                        lux::exec::AsyncCloseReport report) noexcept
                    {
                        close_from_main = report.status;
                        close_completed_from_main.store(
                            true,
                            std::memory_order_release);
                    });
                close_requested_from_main.store(
                    true,
                    std::memory_order_release);
            }),
        "main completion close probe is admitted");
    (void)runtime.drainMainThreadCompletions();
    check(
        close_requested_from_main.load(std::memory_order_acquire) &&
            !runtime.isDrainingMainThreadCompletions(),
        "closeAsync from MainThreadScheduler completion returns without self-wait");
    const auto live_stats = runtime.stats();
    check(
        live_stats.main_queue_high_water >= 1u &&
            live_stats.main_adoption_samples >= 1u &&
            live_stats.main_queue_depth == 0u,
        "MainThreadMailbox reports exact depth, high-water and adoption samples");
    check(
        live_stats.endpoint_queue_wait_samples != 0u &&
            live_stats.coordinator_handler_samples != 0u,
        "typed endpoints report queue wait and coordinator handler samples");
    const auto histogramCount = [](const auto& buckets) noexcept
    {
        std::uint64_t count = 0u;
        for (const auto bucket : buckets)
            count += bucket;
        return count;
    };
    check(
        histogramCount(live_stats.main_adoption_histogram) ==
                live_stats.main_adoption_samples &&
            histogramCount(live_stats.endpoint_queue_wait_histogram) ==
                live_stats.endpoint_queue_wait_samples &&
            histogramCount(live_stats.coordinator_handler_histogram) ==
                live_stats.coordinator_handler_samples,
        "optional latency histograms use fixed allocation-free buckets");

    auto first_close = lux::exec::testing::closeRuntime(runtime);
    check(
        close_completed_from_main.load(std::memory_order_acquire) &&
            (close_from_main == lux::exec::EAsyncCloseStatus::CLOSED ||
             close_from_main ==
                 lux::exec::EAsyncCloseStatus::ALREADY_CLOSED),
        "all repeated close subscribers observe the same terminal state");
    check(
        first_close.clean() &&
            first_close.tracked_operations ==
                first_close.succeeded + first_close.domain_failed +
                    first_close.runtime_failed + first_close.stopped &&
            first_close.active_operations == 0u &&
            first_close.running_operations == 0u &&
            first_close.blocking_io_running == 0u &&
            first_close.background_cpu_running == 0u &&
            first_close.queued_packets == 0u &&
            !first_close.main_completion_pending,
        "close report proves tracked operations reached one terminal");

    lux::exec::AsyncRuntimeBuilder constrained_builder;
    auto constrained_added =
        constrained_builder.addOperation<MissingOperation>(
            [](MissingOperation&&,
               lux::exec::AsyncOperationContext&,
               auto&& completion) noexcept
            {
                completion.complete(1);
            },
            {},
            lux::exec::AsyncOperationQueueConfig{
                .capacity = 1,
                .byte_budget = 8,
                .drain_batch = 1});
    auto constrained_plan = std::move(constrained_builder).compile();
    lux::exec::AsyncRuntime constrained(
        std::move(*constrained_plan),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1,
            .background_cpu_concurrency = 1});
    auto over_budget = wait(lux::exec::execute(
        *constrained_added,
        MissingOperation{},
        lux::exec::AsyncSubmitOptions{
            .accounted_bytes = 9}));
    check(
        over_budget && !std::get<0>(*over_budget) &&
            std::get<0>(*over_budget).error().runtimeError() ==
                lux::exec::EAsyncSubmitError::BYTE_BUDGET_EXHAUSTED,
        "per-operation byte budget rejects without blocking producer");
    const auto constrained_close =
        lux::exec::testing::closeRuntime(constrained);
    check(constrained_close.clean(),
          "independent constrained runtime closes through sender state");

    // A coordinator handler may initiate close, but it must never wait for
    // its own operation state. Use an isolated runtime because initiating
    // close permanently seals admission by design.
    lux::exec::AsyncRuntimeBuilder close_builder;
    auto close_probe = close_builder.addOperation<CloseFromCoordinator>(
        [](CloseFromCoordinator&&,
           lux::exec::AsyncOperationContext& context,
           auto&& completion) noexcept
        {
            lux::exec::detail::subscribeRuntimeClose(
                context.runtime(),
                [](lux::exec::AsyncCloseReport) noexcept {});
            completion.complete(
                lux::exec::EAsyncCloseStatus::CLOSE_IN_PROGRESS);
        }, {}, lux::exec::AsyncOperationQueueConfig{
            .capacity = 16,
            .byte_budget = 1024,
            .drain_batch = 8});
    auto close_plan = std::move(close_builder).compile();
    lux::exec::AsyncRuntime close_runtime(std::move(*close_plan));
    auto close_in_handler = wait(lux::exec::execute(
        *close_probe,
        CloseFromCoordinator{}));
    check(
        close_in_handler && std::get<0>(*close_in_handler).has_value() &&
            *std::get<0>(*close_in_handler) ==
                lux::exec::EAsyncCloseStatus::CLOSE_IN_PROGRESS,
        "close from a coordinator handler initiates without self-wait");
    const auto reentrant_close =
        lux::exec::testing::closeRuntime(close_runtime);
    check(reentrant_close.clean(),
          "coordinator-initiated close reaches one shared terminal");

    lux::exec::AsyncRuntimeBuilder missing_builder;
    lux::exec::AsyncOperationRegistrationOptions missing_options;
    missing_options.prerequisites.push_back(
        lux::exec::kAsyncTypeToken<MissingOperation>);
    (void)missing_builder.addOperation<DoubleValue>(
        [](DoubleValue&&,
           lux::exec::AsyncOperationContext&,
           auto&& completion) noexcept
        {
            completion.failRuntime(
                lux::exec::EAsyncSubmitError::PAYLOAD_INVALID);
        },
        std::move(missing_options));
    auto missing_plan = std::move(missing_builder).compile();
    check(
        !missing_plan && missing_plan.error().code ==
            lux::exec::EAsyncAssemblyError::MISSING_DEPENDENCY,
        "missing operation dependency rejected at freeze time");

    return failures == 0 ? 0 : 1;
}
