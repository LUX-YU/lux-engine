#pragma once
/**
 * @file AsyncOperation.hpp
 * @brief Strongly typed, registration-owned asynchronous operation ingress.
 *
 * Each operation type owns a bounded MPMC queue. Producers move an owning
 * value directly into that queue and only notify the coordinator about the
 * endpoint becoming non-empty. No RTTI, central payload variant, borrowed
 * cross-thread view, or callback task graph is involved.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/runtime/execution/AsyncStatistics.hpp>
#include <lux/cxx/compile_time/type_info.hpp>

#include <moodycamel/concurrentqueue.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace lux::exec
{
    class AsyncRuntime;
    class AsyncScope;
    class MainThreadDispatcher;

    enum class EAsyncSubmitError : std::uint8_t
    {
        UNKNOWN_OPERATION,
        QUEUE_FULL,
        BYTE_BUDGET_EXHAUSTED,
        PAYLOAD_INVALID,
        FEATURE_CLOSING,
        STOPPING
    };

    struct AsyncSubmitOptions final
    {
        std::size_t accounted_bytes{0};
    };

    using AsyncSubmitResult = lux::cxx::expected<void, EAsyncSubmitError>;

    struct AsyncOperationQueueConfig final
    {
        std::size_t capacity{256};
        std::size_t byte_budget{1024u * 1024u};
        std::size_t drain_batch{32};

        [[nodiscard]] bool valid() const noexcept
        {
            return capacity != 0u && byte_budget != 0u &&
                drain_batch != 0u;
        }
    };

    struct AsyncTypeToken final
    {
        std::uint64_t hash{0};
        std::string_view name;

        [[nodiscard]] constexpr bool operator==(
            const AsyncTypeToken&) const noexcept = default;
    };

    template <class T>
    inline constexpr AsyncTypeToken kAsyncTypeToken{
        lux::cxx::type_hash<T>(),
        lux::cxx::type_name<T>()};

    template <class Operation>
    concept AsyncOperation = requires
    {
        typename Operation::Value;
        typename Operation::Error;
    } && std::is_nothrow_move_constructible_v<Operation>;

    template <AsyncOperation Operation>
    class AsyncExecuteSender;

    template <class DomainError>
    class AsyncFailure final
    {
    public:
        [[nodiscard]] static AsyncFailure runtime(
            EAsyncSubmitError error) noexcept
        {
            return AsyncFailure(error);
        }

        [[nodiscard]] static AsyncFailure domain(DomainError error) noexcept
        {
            return AsyncFailure(std::move(error));
        }

        [[nodiscard]] bool isRuntime() const noexcept
        {
            return value_.index() == 0u;
        }

        [[nodiscard]] EAsyncSubmitError runtimeError() const noexcept
        {
            return std::get<0>(value_);
        }

        [[nodiscard]] const DomainError& domainError() const noexcept
        {
            return std::get<1>(value_);
        }

        [[nodiscard]] DomainError& domainError() noexcept
        {
            return std::get<1>(value_);
        }

    private:
        explicit AsyncFailure(EAsyncSubmitError error) noexcept
            : value_(error)
        {}

        explicit AsyncFailure(DomainError error) noexcept
            : value_(std::in_place_index<1>, std::move(error))
        {}

        std::variant<EAsyncSubmitError, DomainError> value_;
    };

    template <AsyncOperation Operation>
    using AsyncOutcome = lux::cxx::expected<typename Operation::Value, AsyncFailure<typename Operation::Error>>;

    class AsyncOperationContext final
    {
    public:
        AsyncOperationContext(
            AsyncRuntime& runtime,
            AsyncScope& scope) noexcept
            : runtime_(&runtime), scope_(&scope)
        {}

        [[nodiscard]] AsyncRuntime& runtime() const noexcept
        {
            return *runtime_;
        }

        [[nodiscard]] AsyncScope& scope() const noexcept
        {
            return *scope_;
        }

        [[nodiscard]] MainThreadDispatcher mainThreadDispatcher() const noexcept;

    private:
        AsyncRuntime* runtime_{nullptr};
        AsyncScope* scope_{nullptr};
    };

    namespace detail
    {
        struct AsyncOperationTracker final
        {
            std::atomic<std::size_t> active{0u};
            std::atomic<std::uint64_t> tracked{0u};
            std::atomic<std::uint64_t> succeeded{0u};
            std::atomic<std::uint64_t> domain_failed{0u};
            std::atomic<std::uint64_t> runtime_failed{0u};
            std::atomic<std::uint64_t> stopped{0u};
            std::atomic<std::uint64_t> queue_wait_samples{0u};
            std::atomic<std::uint64_t> queue_wait_total_ns{0u};
            std::atomic<std::uint64_t> queue_wait_max_ns{0u};
            std::atomic<std::uint64_t> handler_samples{0u};
            std::atomic<std::uint64_t> handler_total_ns{0u};
            std::atomic<std::uint64_t> handler_max_ns{0u};
            std::array<
                std::atomic<std::uint64_t>,
                kAsyncLatencyBucketCount> queue_wait_histogram{};
            std::array<
                std::atomic<std::uint64_t>,
                kAsyncLatencyBucketCount> handler_histogram{};
            bool histograms_enabled{false};

            static void record(
                std::atomic<std::uint64_t>& samples,
                std::atomic<std::uint64_t>& total,
                std::atomic<std::uint64_t>& maximum,
                std::uint64_t value) noexcept
            {
                samples.fetch_add(1u, std::memory_order_relaxed);
                total.fetch_add(value, std::memory_order_relaxed);
                auto high = maximum.load(std::memory_order_relaxed);
                while (high < value &&
                       !maximum.compare_exchange_weak(
                           high,
                           value,
                           std::memory_order_relaxed,
                           std::memory_order_relaxed))
                {}
            }

            void recordQueueWait(std::uint64_t value) noexcept
            {
                record(
                    queue_wait_samples,
                    queue_wait_total_ns,
                    queue_wait_max_ns,
                    value
                );
                if (histograms_enabled)
                {
                    queue_wait_histogram[asyncLatencyBucket(value)].fetch_add(
                        1u,
                        std::memory_order_relaxed);
                }
            }

            void recordHandler(std::uint64_t value) noexcept
            {
                record(
                    handler_samples,
                    handler_total_ns,
                    handler_max_ns,
                    value
                );
                if (histograms_enabled)
                {
                    handler_histogram[asyncLatencyBucket(value)].fetch_add(
                        1u,
                        std::memory_order_relaxed);
                }
            }
        };

        [[nodiscard]] inline std::uint64_t asyncSteadyNowNs() noexcept
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );
        }

        template <AsyncOperation Operation>
        class AsyncCompletion final
        {
        public:
            using Outcome = AsyncOutcome<Operation>;

            AsyncCompletion() noexcept = default;
            explicit AsyncCompletion(
                lux::cxx::move_only_function<void(void*)> complete) noexcept
                : complete_(std::move(complete))
            {}

            AsyncCompletion(
                lux::cxx::move_only_function<void(void*)> complete,
                std::shared_ptr<const void> module_lease,
                std::shared_ptr<AsyncOperationTracker> tracker) noexcept
                : complete_(std::move(complete))
                , module_lease_(std::move(module_lease))
                , tracker_(std::move(tracker))
            {}

            ~AsyncCompletion()
            {
                if (complete_)
                    failRuntime(EAsyncSubmitError::PAYLOAD_INVALID);
            }

            AsyncCompletion(const AsyncCompletion&) = delete;
            AsyncCompletion& operator=(const AsyncCompletion&) = delete;
            AsyncCompletion(AsyncCompletion&& other) noexcept
                : complete_(std::move(other.complete_))
                , module_lease_(std::move(other.module_lease_))
                , tracker_(std::move(other.tracker_))
            {}

            AsyncCompletion& operator=(AsyncCompletion&& other) noexcept
            {
                if (this == &other)
                    return *this;
                if (complete_)
                    failRuntime(EAsyncSubmitError::PAYLOAD_INVALID);
                complete_ = std::move(other.complete_);
                module_lease_ = std::move(other.module_lease_);
                tracker_ = std::move(other.tracker_);
                return *this;
            }

            void complete(Outcome outcome) noexcept
            {
                if (!complete_)
                    return;
                auto complete = std::move(complete_);
                auto tracker = std::move(tracker_);
                auto module_lease = std::move(module_lease_);
                if (tracker)
                {
                    if (outcome)
                        tracker->succeeded.fetch_add(1u, std::memory_order_relaxed);
                    else if (outcome.error().isRuntime())
                    {
                        const auto error = outcome.error().runtimeError();
                        if (error == EAsyncSubmitError::STOPPING ||
                            error == EAsyncSubmitError::FEATURE_CLOSING)
                        {
                            tracker->stopped.fetch_add(
                                1u,
                                std::memory_order_relaxed);
                        }
                        else
                        {
                            tracker->runtime_failed.fetch_add(
                                1u,
                                std::memory_order_relaxed);
                        }
                    }
                    else
                        tracker->domain_failed.fetch_add(1u, std::memory_order_relaxed);
                }
                complete(&outcome);
                if (tracker)
                    tracker->active.fetch_sub(1u, std::memory_order_release);
            }

            void failRuntime(EAsyncSubmitError error) noexcept
            {
                complete(lux::cxx::unexpected(
                    AsyncFailure<typename Operation::Error>::runtime(error)));
            }

            [[nodiscard]] explicit operator bool() const noexcept
            {
                return static_cast<bool>(complete_);
            }

        private:
            lux::cxx::move_only_function<void(void*)> complete_;
            std::shared_ptr<const void> module_lease_;
            std::shared_ptr<AsyncOperationTracker> tracker_;
        };

        enum class EAsyncEndpointState : std::uint8_t
        {
            INACTIVE,
            OPEN,
            CLOSING,
            STOPPING
        };

        class AsyncEndpointRuntimeControl;
        class AsyncEndpointBase;

        [[nodiscard]] bool notifyEndpoint(
            const std::weak_ptr<AsyncEndpointRuntimeControl>& control,
            std::shared_ptr<AsyncEndpointBase> endpoint) noexcept;

        [[nodiscard]] bool notifyClosingEndpoint(
            const std::weak_ptr<AsyncEndpointRuntimeControl>& control,
            std::shared_ptr<AsyncEndpointBase> endpoint) noexcept;

        class AsyncEndpointBase
            : public std::enable_shared_from_this<AsyncEndpointBase>
        {
        public:
            class AdmissionTicket final
            {
            public:
                AdmissionTicket() noexcept = default;
                AdmissionTicket(const AdmissionTicket&) = delete;
                AdmissionTicket& operator=(const AdmissionTicket&) = delete;

                AdmissionTicket(AdmissionTicket&& other) noexcept
                    : endpoint_(std::exchange(other.endpoint_, nullptr))
                    , control_(std::move(other.control_))
                {}

                AdmissionTicket& operator=(AdmissionTicket&& other) noexcept
                {
                    if (this == &other)
                        return *this;
                    release();
                    endpoint_ = std::exchange(other.endpoint_, nullptr);
                    control_ = std::move(other.control_);
                    return *this;
                }

                ~AdmissionTicket() { release(); }

                [[nodiscard]] explicit operator bool() const noexcept
                {
                    return endpoint_ != nullptr;
                }

            private:
                friend class AsyncEndpointBase;

                AdmissionTicket(
                    AsyncEndpointBase* endpoint,
                    std::weak_ptr<AsyncEndpointRuntimeControl> control)
                    noexcept
                    : endpoint_(endpoint)
                    , control_(std::move(control))
                {}

                void release() noexcept
                {
                    if (endpoint_ != nullptr)
                    {
                        std::exchange(endpoint_, nullptr)->releaseAdmission(
                            std::move(control_));
                    }
                }

                AsyncEndpointBase* endpoint_{nullptr};
                std::weak_ptr<AsyncEndpointRuntimeControl> control_;
            };

            AsyncEndpointBase(
                AsyncTypeToken type,
                AsyncOperationQueueConfig config) noexcept
                : type_(type), config_(config)
            {}

            AsyncEndpointBase(const AsyncEndpointBase&) = delete;
            AsyncEndpointBase& operator=(const AsyncEndpointBase&) = delete;
            virtual ~AsyncEndpointBase() = default;

            [[nodiscard]] AsyncTypeToken type() const noexcept { return type_; }
            [[nodiscard]] std::size_t drainBatch() const noexcept
            {
                return config_.drain_batch;
            }
            [[nodiscard]] std::size_t queued() const noexcept
            {
                return queued_count_.load(std::memory_order_acquire);
            }
            [[nodiscard]] std::size_t queuedBytes() const noexcept
            {
                return queued_bytes_.load(std::memory_order_acquire);
            }
            [[nodiscard]] std::size_t queueHighWater() const noexcept
            {
                return queue_high_water_.load(std::memory_order_relaxed);
            }
            [[nodiscard]] std::size_t byteHighWater() const noexcept
            {
                return byte_high_water_.load(std::memory_order_relaxed);
            }

            void recordQueueWait(std::uint64_t nanoseconds) noexcept
            {
                if (tracker_)
                    tracker_->recordQueueWait(nanoseconds);
            }

            void recordHandler(std::uint64_t nanoseconds) noexcept
            {
                if (tracker_)
                    tracker_->recordHandler(nanoseconds);
            }

            void activate(
                std::weak_ptr<AsyncEndpointRuntimeControl> control,
                std::shared_ptr<const void> module_lease,
                std::shared_ptr<AsyncOperationTracker> tracker) noexcept
            {
                control_ = std::move(control);
                module_lease_ = std::move(module_lease);
                tracker_ = std::move(tracker);
                admission_gate_.store(
                    encode(EAsyncEndpointState::OPEN, 0u),
                    std::memory_order_release);
            }

            void closeAdmission(bool stopping) noexcept
            {
                const auto requested = stopping
                    ? EAsyncEndpointState::STOPPING
                    : EAsyncEndpointState::CLOSING;
                auto gate = admission_gate_.load(std::memory_order_acquire);
                for (;;)
                {
                    const auto current = decodeState(gate);
                    if (current == EAsyncEndpointState::INACTIVE ||
                        current == EAsyncEndpointState::STOPPING ||
                        (current == EAsyncEndpointState::CLOSING && !stopping))
                        return;
                    const auto desired = encode(requested, producerCount(gate));
                    if (admission_gate_.compare_exchange_weak(
                            gate,
                            desired,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire))
                        return;
                }
            }

            [[nodiscard]] bool deactivate() noexcept
            {
                const auto gate = admission_gate_.load(std::memory_order_acquire);
                if (producerCount(gate) != 0u)
                    return false;
                control_.reset();
                module_lease_.reset();
                tracker_.reset();
                admission_gate_.store(
                    encode(EAsyncEndpointState::INACTIVE, 0u),
                    std::memory_order_release);
                return true;
            }

            [[nodiscard]] AdmissionTicket tryAcquireAdmission() noexcept
            {
                auto gate = admission_gate_.load(std::memory_order_acquire);
                while (decodeState(gate) == EAsyncEndpointState::OPEN)
                {
                    const auto producers = producerCount(gate);
                    if (producers == kProducerMask)
                        return {};
                    if (admission_gate_.compare_exchange_weak(
                            gate,
                            encode(EAsyncEndpointState::OPEN, producers + 1u),
                            std::memory_order_acq_rel,
                            std::memory_order_acquire))
                        return AdmissionTicket{this, control_};
                }
                return {};
            }

            [[nodiscard]] std::size_t activeProducers() const noexcept
            {
                return producerCount(
                    admission_gate_.load(std::memory_order_acquire));
            }

            [[nodiscard]] EAsyncEndpointState state() const noexcept
            {
                return decodeState(
                    admission_gate_.load(std::memory_order_acquire));
            }

            [[nodiscard]] bool markScheduled() noexcept
            {
                return !scheduled_.exchange(true, std::memory_order_acq_rel);
            }

            void clearScheduled() noexcept
            {
                scheduled_.store(false, std::memory_order_release);
            }

            [[nodiscard]] bool isScheduled() const noexcept
            {
                return scheduled_.load(std::memory_order_acquire);
            }

            [[nodiscard]] std::weak_ptr<AsyncEndpointRuntimeControl>
            runtimeControl() const noexcept
            {
                return control_;
            }

        protected:
            [[nodiscard]] EAsyncSubmitError admissionError() const noexcept
            {
                switch (state())
                {
                case EAsyncEndpointState::OPEN:
                    return EAsyncSubmitError::PAYLOAD_INVALID;
                case EAsyncEndpointState::CLOSING:
                    return EAsyncSubmitError::FEATURE_CLOSING;
                case EAsyncEndpointState::STOPPING:
                    return EAsyncSubmitError::STOPPING;
                case EAsyncEndpointState::INACTIVE:
                    return EAsyncSubmitError::UNKNOWN_OPERATION;
                }
                return EAsyncSubmitError::STOPPING;
            }

            enum class EReservationResult : std::uint8_t
            {
                RESERVED,
                QUEUE_FULL,
                BYTE_BUDGET_EXHAUSTED
            };

            [[nodiscard]] EReservationResult reserve(
                std::size_t bytes) noexcept
            {
                auto count = queued_count_.load(std::memory_order_relaxed);
                for (;;)
                {
                    if (count >= config_.capacity)
                        return EReservationResult::QUEUE_FULL;
                    if (queued_count_.compare_exchange_weak(
                            count, count + 1u,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed))
                        break;
                }

                auto used = queued_bytes_.load(std::memory_order_relaxed);
                for (;;)
                {
                    if (bytes > config_.byte_budget ||
                        used > config_.byte_budget - bytes)
                    {
                        queued_count_.fetch_sub(1u, std::memory_order_release);
                        return EReservationResult::BYTE_BUDGET_EXHAUSTED;
                    }
                    if (queued_bytes_.compare_exchange_weak(
                            used, used + bytes,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed))
                        break;
                }
                updateHighWater(queue_high_water_, count + 1u);
                updateHighWater(byte_high_water_, used + bytes);
                return EReservationResult::RESERVED;
            }

            void release(std::size_t bytes) noexcept
            {
                queued_bytes_.fetch_sub(bytes, std::memory_order_release);
                queued_count_.fetch_sub(1u, std::memory_order_release);
            }

            [[nodiscard]] const AsyncOperationQueueConfig& config() const noexcept
            {
                return config_;
            }
            [[nodiscard]] const std::shared_ptr<const void>& moduleLease() const noexcept
            {
                return module_lease_;
            }
            [[nodiscard]] const std::shared_ptr<AsyncOperationTracker>& tracker() const noexcept
            {
                return tracker_;
            }
            [[nodiscard]] std::uint64_t nextRequestId() noexcept
            {
                return next_request_id_.fetch_add(1u, std::memory_order_relaxed);
            }

        private:
            static constexpr std::uint64_t kStateBits = 2u;
            static constexpr std::uint64_t kStateMask =
                (std::uint64_t{1u} << kStateBits) - 1u;
            static constexpr std::uint64_t kProducerMask =
                std::numeric_limits<std::uint64_t>::max() >> kStateBits;

            [[nodiscard]] static constexpr std::uint64_t encode(
                EAsyncEndpointState state,
                std::uint64_t producers) noexcept
            {
                return (producers << kStateBits) |
                    static_cast<std::uint64_t>(state);
            }

            [[nodiscard]] static constexpr EAsyncEndpointState decodeState(
                std::uint64_t gate) noexcept
            {
                return static_cast<EAsyncEndpointState>(gate & kStateMask);
            }

            [[nodiscard]] static constexpr std::uint64_t producerCount(
                std::uint64_t gate) noexcept
            {
                return gate >> kStateBits;
            }

            void releaseAdmission(
                std::weak_ptr<AsyncEndpointRuntimeControl> control) noexcept
            {
                const auto previous = admission_gate_.fetch_sub(
                    std::uint64_t{1u} << kStateBits,
                    std::memory_order_acq_rel);
                if (producerCount(previous) != 1u)
                    return;
                const auto endpoint_state = decodeState(previous);
                if (endpoint_state != EAsyncEndpointState::CLOSING &&
                    endpoint_state != EAsyncEndpointState::STOPPING)
                    return;
                auto self = weak_from_this().lock();
                if (self)
                    (void)notifyClosingEndpoint(
                        control,
                        std::move(self));
            }

            static void updateHighWater(
                std::atomic<std::size_t>& target,
                std::size_t value) noexcept
            {
                auto high = target.load(std::memory_order_relaxed);
                while (high < value &&
                       !target.compare_exchange_weak(
                           high, value,
                           std::memory_order_relaxed,
                           std::memory_order_relaxed))
                {}
            }

            AsyncTypeToken type_{};
            AsyncOperationQueueConfig config_{};
            std::atomic<std::uint64_t> admission_gate_{
                encode(EAsyncEndpointState::INACTIVE, 0u)};
            std::atomic<bool> scheduled_{false};
            std::atomic<std::size_t> queued_count_{0u};
            std::atomic<std::size_t> queued_bytes_{0u};
            std::atomic<std::size_t> queue_high_water_{0u};
            std::atomic<std::size_t> byte_high_water_{0u};
            std::atomic<std::uint64_t> next_request_id_{1u};
            std::weak_ptr<AsyncEndpointRuntimeControl> control_;
            std::shared_ptr<const void> module_lease_;
            std::shared_ptr<AsyncOperationTracker> tracker_;
        };

        template <AsyncOperation Operation>
        class OperationEndpoint final : public AsyncEndpointBase
        {
        public:
            using Outcome = AsyncOutcome<Operation>;

            explicit OperationEndpoint(AsyncOperationQueueConfig config)
                : AsyncEndpointBase(kAsyncTypeToken<Operation>, config)
                , queue_(config.capacity)
            {}

            struct Queued final
            {
                Queued() noexcept = default;
                Queued(
                    Operation value,
                    void* state,
                    void (*terminal)(void*, Outcome&&) noexcept,
                    std::uint64_t request,
                    std::size_t bytes,
                    std::shared_ptr<const void> module,
                    std::shared_ptr<AsyncOperationTracker> operation_tracker) noexcept
                    : operation(std::move(value))
                    , completion_state(state)
                    , complete(terminal)
                    , request_id(request)
                    , accounted_bytes(bytes)
                    , enqueued_ns(asyncSteadyNowNs())
                    , module_lease(std::move(module))
                    , tracker(std::move(operation_tracker))
                {
                    if (tracker)
                    {
                        tracker->tracked.fetch_add(1u, std::memory_order_relaxed);
                        tracker->active.fetch_add(1u, std::memory_order_relaxed);
                    }
                }

                ~Queued()
                {
                    if (complete)
                        reject(EAsyncSubmitError::STOPPING);
                }

                Queued(const Queued&) = delete;
                Queued& operator=(const Queued&) = delete;
                Queued(Queued&& other) noexcept
                    : operation(std::move(other.operation))
                    , completion_state(std::exchange(other.completion_state, nullptr))
                    , complete(std::exchange(other.complete, nullptr))
                    , request_id(other.request_id)
                    , accounted_bytes(other.accounted_bytes)
                    , enqueued_ns(other.enqueued_ns)
                    , module_lease(std::move(other.module_lease))
                    , tracker(std::move(other.tracker))
                {}

                Queued& operator=(Queued&& other) noexcept
                {
                    if (this == &other)
                        return *this;
                    if (complete)
                        reject(EAsyncSubmitError::STOPPING);
                    operation = std::move(other.operation);
                    completion_state = std::exchange(other.completion_state, nullptr);
                    complete = std::exchange(other.complete, nullptr);
                    request_id = other.request_id;
                    accounted_bytes = other.accounted_bytes;
                    enqueued_ns = other.enqueued_ns;
                    module_lease = std::move(other.module_lease);
                    tracker = std::move(other.tracker);
                    return *this;
                }

                [[nodiscard]] AsyncCompletion<Operation> takeCompletion() noexcept
                {
                    auto state = std::exchange(completion_state, nullptr);
                    auto terminal = std::exchange(complete, nullptr);
                    return AsyncCompletion<Operation>{
                        [state, terminal](void* outcome) noexcept
                        {
                            terminal(
                                state,
                                std::move(*static_cast<Outcome*>(outcome)));
                        },
                        std::move(module_lease),
                        std::move(tracker)};
                }

                void reject(EAsyncSubmitError error) noexcept
                {
                    auto completion = takeCompletion();
                    completion.failRuntime(error);
                }

                // Keeping the dequeue target disengaged removes a hidden
                // default-construction requirement from AsyncOperation. The
                // queue itself still moves the concrete, strongly typed value.
                std::optional<Operation> operation;
                void* completion_state{nullptr};
                void (*complete)(void*, Outcome&&) noexcept{nullptr};
                std::uint64_t request_id{0u};
                std::size_t accounted_bytes{0u};
                std::uint64_t enqueued_ns{0u};
                std::shared_ptr<const void> module_lease;
                std::shared_ptr<AsyncOperationTracker> tracker;
            };

            [[nodiscard]] AsyncSubmitResult submit(
                Operation operation,
                void* completion_state,
                void (*complete)(void*, Outcome&&) noexcept,
                AsyncSubmitOptions options) noexcept
            {
                auto admission = tryAcquireAdmission();
                if (!admission)
                {
                    const auto error = admissionError();
                    complete(
                        completion_state,
                        lux::cxx::unexpected(
                            AsyncFailure<typename Operation::Error>::runtime(error)));
                    return lux::cxx::unexpected(error);
                }
                const auto reservation = reserve(options.accounted_bytes);
                if (reservation != EReservationResult::RESERVED)
                {
                    const auto error =
                        reservation == EReservationResult::QUEUE_FULL
                            ? EAsyncSubmitError::QUEUE_FULL
                            : EAsyncSubmitError::BYTE_BUDGET_EXHAUSTED;
                    complete(
                        completion_state,
                        lux::cxx::unexpected(
                            AsyncFailure<typename Operation::Error>::runtime(error)));
                    return lux::cxx::unexpected(error);
                }

                Queued value{
                    std::move(operation),
                    completion_state,
                    complete,
                    nextRequestId(),
                    options.accounted_bytes,
                    moduleLease(),
                    tracker()};
                if (!queue_.try_enqueue(std::move(value)))
                {
                    release(options.accounted_bytes);
                    value.reject(EAsyncSubmitError::QUEUE_FULL);
                    return lux::cxx::unexpected(EAsyncSubmitError::QUEUE_FULL);
                }
                auto self = weak_from_this().lock();
                if (self)
                    (void)notifyEndpoint(runtimeControl(), std::move(self));
                return {};
            }

            [[nodiscard]] bool tryTake(Queued& value) noexcept
            {
                if (!queue_.try_dequeue(value))
                    return false;
                release(value.accounted_bytes);
                return true;
            }

            void rejectAll(EAsyncSubmitError error) noexcept
            {
                Queued value;
                while (tryTake(value))
                {
                    value.reject(error);
                    value = {};
                }
            }

        private:
            moodycamel::ConcurrentQueue<Queued> queue_;
        };
    }

    template <AsyncOperation Operation>
    using AsyncOperationCompletion = detail::AsyncCompletion<Operation>;

    template <AsyncOperation Operation>
    class AsyncOperationClient final
    {
    public:
        using Endpoint = detail::OperationEndpoint<Operation>;
        using Outcome = AsyncOutcome<Operation>;

        AsyncOperationClient() noexcept = default;

        /// Fire-and-forget is only meaningful for notification-shaped
        /// operations. Work with a value must use execute() so its terminal
        /// result cannot be silently discarded.
        [[nodiscard]] AsyncSubmitResult tryNotify(
            Operation operation,
            AsyncSubmitOptions options = {}) const noexcept
            requires std::is_void_v<typename Operation::Value>
        {
            static std::byte completion_anchor;
            return submit(
                std::move(operation),
                &completion_anchor,
                +[](void*, Outcome&&) noexcept {},
                options);
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(endpoint_);
        }

    private:
        template <AsyncOperation OtherOperation>
        friend class AsyncExecuteSender;
        friend class AsyncRuntimeBuilder;

        explicit AsyncOperationClient(std::shared_ptr<Endpoint> endpoint) noexcept
            : endpoint_(std::move(endpoint))
        {}

        [[nodiscard]] AsyncSubmitResult submit(
            Operation operation,
            void* completion_state,
            void (*complete)(void*, Outcome&&) noexcept,
            AsyncSubmitOptions options) const noexcept
        {
            if (!endpoint_)
            {
                complete(
                    completion_state,
                    lux::cxx::unexpected(
                        AsyncFailure<typename Operation::Error>::runtime(
                            EAsyncSubmitError::UNKNOWN_OPERATION)));
                return lux::cxx::unexpected(
                    EAsyncSubmitError::UNKNOWN_OPERATION);
            }
            return endpoint_->submit(
                std::move(operation), completion_state, complete, options);
        }

        std::shared_ptr<Endpoint> endpoint_;
    };
}
