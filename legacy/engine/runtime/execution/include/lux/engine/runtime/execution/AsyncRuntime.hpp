#pragma once
/**
 * @file AsyncRuntime.hpp
 * @brief Process-wide stdexec orchestration runtime.
 *
 * A single standalone-Asio context owns coordination, infrastructure timers
 * and dynamic registrations. Operation payloads live in
 * registration-owned typed queues; CPU work executes in oneTBB and blocking
 * compatibility IO has a deliberately separate, small executor.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/runtime/execution/AsyncOperation.hpp>

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace experimental::execution
{
    struct static_thread_pool;
    namespace tbb { class tbb_thread_pool; }
}

namespace lux::exec
{
    class MainThreadMailbox;
    class AsyncRuntime;
    class AsyncFileService;
    class AsyncClient;
    class AsyncClientControl;
    class CoordinatorScheduler;
    class AsyncRuntimePlan;
    class AsyncRuntimeBuilder;
    class AsyncOperationBundle;
    class AsyncOperationBundleInstallSender;
    class AsyncOperationBundleCloseSender;
    class AsyncRuntimeCloseSender;
    class BlockingIoScheduler;
    class BackgroundCpuScheduler;

    namespace detail
    {
        class CoordinatorSignalState;

        struct CoordinatorTimerState final
        {
            std::atomic<bool> cancelled{false};
            std::atomic<std::uint64_t> id{0u};
        };
    }

    struct AsyncRuntimeConfig final
    {
        std::size_t blocking_io_threads{0};
        std::size_t background_cpu_concurrency{0};
        std::size_t main_thread_drain_budget{4096};
        bool enable_latency_histograms{false};
    };

    enum class EAsyncCloseStatus : std::uint8_t
    {
        CLOSED,
        ALREADY_CLOSED,
        CLOSE_IN_PROGRESS
    };

    struct AsyncRuntimeStats
    {
        std::uint64_t accepted{0};
        std::uint64_t dispatched{0};
        std::uint64_t rejected{0};
        std::uint64_t tracked_operations{0};
        std::uint64_t succeeded{0};
        std::uint64_t domain_failed{0};
        std::uint64_t runtime_failed{0};
        std::uint64_t stopped{0};
        std::uint64_t wakeups{0};
        std::size_t active_operations{0};
        std::size_t running_operations{0};
        std::size_t queued_packets{0};
        std::size_t scheduled_queued_endpoints{0};
        std::size_t unscheduled_queued_endpoints{0};
        std::size_t queued_bytes{0};
        std::size_t queue_high_water{0};
        std::size_t byte_high_water{0};
        std::size_t pending_timers{0};
        std::size_t asio_file_pending{0u};
        std::size_t asio_native_file_requests{0u};
        std::size_t blocking_io_running{0u};
        std::size_t background_cpu_running{0u};
        std::uint64_t file_request_state_allocations{0u};
        std::uint64_t file_pending_state_allocations{0u};
        std::uint64_t file_native_state_allocations{0u};
        bool main_completion_pending{false};
        std::size_t main_queue_depth{0u};
        std::size_t main_queue_high_water{0u};
        std::uint64_t main_oldest_age_ns{0u};
        std::uint64_t main_adoption_samples{0u};
        std::uint64_t main_adoption_total_ns{0u};
        std::uint64_t main_adoption_max_ns{0u};
        std::uint64_t endpoint_queue_wait_samples{0u};
        std::uint64_t endpoint_queue_wait_total_ns{0u};
        std::uint64_t endpoint_queue_wait_max_ns{0u};
        std::uint64_t coordinator_handler_samples{0u};
        std::uint64_t coordinator_handler_total_ns{0u};
        std::uint64_t coordinator_handler_max_ns{0u};
        AsyncLatencyHistogram main_adoption_histogram{};
        AsyncLatencyHistogram endpoint_queue_wait_histogram{};
        AsyncLatencyHistogram coordinator_handler_histogram{};
    };

    struct AsyncCloseReport final : AsyncRuntimeStats
    {
        EAsyncCloseStatus status{EAsyncCloseStatus::CLOSED};

        [[nodiscard]] bool clean() const noexcept
        {
            return (status == EAsyncCloseStatus::CLOSED ||
                    status == EAsyncCloseStatus::ALREADY_CLOSED) &&
                active_operations == 0u && queued_packets == 0u &&
                queued_bytes == 0u && pending_timers == 0u &&
                asio_file_pending == 0u &&
                asio_native_file_requests == 0u &&
                blocking_io_running == 0u &&
                background_cpu_running == 0u &&
                !main_completion_pending &&
                tracked_operations ==
                    succeeded + domain_failed + runtime_failed + stopped;
        }
    };

    namespace detail
    {
        void subscribeRuntimeClose(
            AsyncRuntime& runtime,
            lux::cxx::move_only_function<void(AsyncCloseReport)> completion)
            noexcept;
    }

    enum class EAsyncJoinError : std::uint8_t
    {
        CLOSE_NOT_COMPLETE,
        WRONG_THREAD,
        ALREADY_JOINED
    };

    enum class EAsyncOperationBundleInstallError : std::uint8_t
    {
        DUPLICATE_OPERATION,
        MISSING_DEPENDENCY,
        TYPE_COLLISION,
        INVALID_PACKAGE,
        STOPPING
    };

    enum class EAsyncOperationBundleCloseError : std::uint8_t
    {
        UNKNOWN_BUNDLE,
        CLOSE_IN_PROGRESS,
        STOPPING
    };

    struct AsyncOperationBundleCloseReport final
    {
        std::size_t removed_operations{0};
        std::size_t stopped_operations{0};
    };

    /// Internal orchestration handle. Business code receives typed operation
    /// clients instead; this handle only backs stdexec coordinator scheduling
    /// and dynamic operation bundle administration.
    class AsyncClient final
    {
    public:
        AsyncClient() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return !control_.expired();
        }

        [[nodiscard]] bool operator==(const AsyncClient& other) const noexcept
        {
            return !control_.owner_before(other.control_) &&
                !other.control_.owner_before(control_);
        }

    private:
        friend class AsyncRuntime;
        friend class CoordinatorScheduler;
        friend class CoordinatorTimerSender;
        friend class AsyncOperationBundleLease;
        friend class AsyncOperationBundleInstallSender;
        friend class AsyncOperationBundleCloseSender;

        explicit AsyncClient(std::weak_ptr<AsyncClientControl> control) noexcept
            : control_(std::move(control))
        {}

        [[nodiscard]] bool tryDispatchToMainThreadContinuation(
            lux::cxx::move_only_function<void()> task) const noexcept;
        [[nodiscard]] bool tryScheduleAfter(
            std::chrono::steady_clock::duration delay,
            std::shared_ptr<detail::CoordinatorTimerState> state,
            lux::cxx::move_only_function<void(bool)> completion) const noexcept;
        void cancelScheduledTimer(
            const std::shared_ptr<detail::CoordinatorTimerState>& state) const
            noexcept;
        [[nodiscard]] bool tryInstallOperations(
            AsyncOperationBundle&& package,
            lux::cxx::move_only_function<void(
                lux::cxx::expected<std::uint64_t, EAsyncOperationBundleInstallError>)>
                completion) const noexcept;
        [[nodiscard]] bool tryCloseOperations(
            std::uint64_t bundle_id,
            lux::cxx::move_only_function<void(
                lux::cxx::expected<
                    AsyncOperationBundleCloseReport,
                    EAsyncOperationBundleCloseError>)> completion) const noexcept;
        void closeOperationsDetached(std::uint64_t bundle_id) const noexcept;

        std::weak_ptr<AsyncClientControl> control_;
    };

    class AsyncOperationBundleLease final
    {
    public:
        AsyncOperationBundleLease() noexcept = default;
        ~AsyncOperationBundleLease() { reset(); }
        AsyncOperationBundleLease(const AsyncOperationBundleLease&) = delete;
        AsyncOperationBundleLease& operator=(const AsyncOperationBundleLease&) = delete;
        AsyncOperationBundleLease(AsyncOperationBundleLease&& other) noexcept
            : client_(std::move(other.client_))
            , bundle_id_(std::exchange(other.bundle_id_, 0u))
        {}
        AsyncOperationBundleLease& operator=(AsyncOperationBundleLease&& other) noexcept
        {
            if (this == &other)
                return *this;
            reset();
            client_ = std::move(other.client_);
            bundle_id_ = std::exchange(other.bundle_id_, 0u);
            return *this;
        }

        [[nodiscard]] bool active() const noexcept { return bundle_id_ != 0u; }
        void reset() noexcept
        {
            if (bundle_id_ == 0u)
                return;
            client_.closeOperationsDetached(bundle_id_);
            bundle_id_ = 0u;
            client_ = {};
        }

    private:
        friend class AsyncOperationBundleInstallSender;
        friend class AsyncOperationBundleCloseSender;
        friend class AsyncRuntime;
        AsyncOperationBundleLease(AsyncClient client, std::uint64_t bundle_id) noexcept
            : client_(std::move(client)), bundle_id_(bundle_id)
        {}
        [[nodiscard]] std::uint64_t release() noexcept
        {
            client_ = {};
            return std::exchange(bundle_id_, 0u);
        }
        AsyncClient client_;
        std::uint64_t bundle_id_{0};
    };

    class MainThreadDispatcher final
    {
    public:
        MainThreadDispatcher() noexcept = default;
        [[nodiscard]] bool tryDispatchToMainThread(
            lux::cxx::move_only_function<void()> task) const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return !control_.expired();
        }

    private:
        friend class AsyncRuntime;
        explicit MainThreadDispatcher(std::weak_ptr<AsyncClientControl> control) noexcept
            : control_(std::move(control))
        {}
        std::weak_ptr<AsyncClientControl> control_;
    };

    /// Coalesced external-source notification. The payload remains in the
    /// source-owned queue; Asio receives only this lightweight edge.
    class CoordinatorSignal final
    {
    public:
        CoordinatorSignal() noexcept = default;
        [[nodiscard]] bool notify() const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(state_);
        }

    private:
        friend class AsyncRuntime;
        explicit CoordinatorSignal(
            std::shared_ptr<detail::CoordinatorSignalState> state) noexcept
            : state_(std::move(state))
        {}
        std::shared_ptr<detail::CoordinatorSignalState> state_;
    };

    class AsyncRuntime final
    {
    public:
        explicit AsyncRuntime(AsyncRuntimeConfig config = {});
        AsyncRuntime(AsyncRuntimePlan&& plan, AsyncRuntimeConfig config = {});
        ~AsyncRuntime();

        AsyncRuntime(const AsyncRuntime&) = delete;
        AsyncRuntime& operator=(const AsyncRuntime&) = delete;
        AsyncRuntime(AsyncRuntime&&) = delete;
        AsyncRuntime& operator=(AsyncRuntime&&) = delete;

        [[nodiscard]] AsyncClient client() noexcept;
        [[nodiscard]] MainThreadDispatcher mainThreadDispatcher() noexcept;
        [[nodiscard]] CoordinatorSignal makeCoordinatorSignal(
            lux::cxx::move_only_function<void()> handler) noexcept;
        [[nodiscard]] AsyncFileService& fileService() noexcept;

        std::size_t drainMainThreadCompletions(
            std::size_t budget = static_cast<std::size_t>(-1));
        [[nodiscard]] bool isDrainingMainThreadCompletions() const noexcept;
        [[nodiscard]] AsyncRuntimeStats stats() const noexcept;
        [[nodiscard]] bool latencyHistogramsEnabled() const noexcept;
        [[nodiscard]] AsyncRuntimeCloseSender closeAsync() noexcept;
        [[nodiscard]] lux::cxx::expected<void, EAsyncJoinError>
        join() noexcept;
        [[nodiscard]] AsyncOperationBundleInstallSender installOperations(
            AsyncOperationBundle package) noexcept;
        [[nodiscard]] AsyncOperationBundleCloseSender closeOperations(
            AsyncOperationBundleLease&& lease) noexcept;

        [[nodiscard]] ::experimental::execution::static_thread_pool&
        blockingIoPool() noexcept;
        [[nodiscard]] ::experimental::execution::tbb::tbb_thread_pool&
        backgroundCpuPool() noexcept;
        [[nodiscard]] MainThreadMailbox& mainThreadMailbox() noexcept;

    private:
        friend class AsyncClientControl;
        friend class AsyncOperationBundleInstallSender;
        friend class AsyncOperationBundleCloseSender;
        friend class AsyncRuntimeCloseSender;
        friend class BlockingIoScheduler;
        friend class BackgroundCpuScheduler;
        friend class AsyncRuntimeBuilder;
        friend class detail::AsyncEndpointRuntimeControl;
        friend void detail::subscribeRuntimeClose(
            AsyncRuntime&,
            lux::cxx::move_only_function<void(AsyncCloseReport)>) noexcept;

        struct Impl;
        void subscribeClose(
            lux::cxx::move_only_function<void(AsyncCloseReport)> completion)
            noexcept;

        [[nodiscard]] std::shared_ptr<std::atomic<std::size_t>>
        blockingIoActivityCounter() noexcept;
        [[nodiscard]] std::shared_ptr<std::atomic<std::size_t>>
        backgroundCpuActivityCounter() noexcept;
        std::unique_ptr<Impl> impl_;
        std::shared_ptr<AsyncClientControl> client_control_;
        std::unique_ptr<AsyncFileService> file_service_;
    };
}
