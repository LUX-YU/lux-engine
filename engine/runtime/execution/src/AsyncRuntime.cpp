#include <lux/engine/runtime/execution/AsyncRuntime.hpp>

#include <lux/engine/runtime/execution/AsyncFileService.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/detail/AsioConfig.hpp>
#include <lux/engine/runtime/execution/detail/MainThreadScheduler.hpp>

#include "AsyncFileControlFactory.hpp"

#include <asio/error.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <exec/static_thread_pool.hpp>
#include <exec/start_detached.hpp>
#include <exec/tbb/tbb_thread_pool.hpp>
#include <stdexec/execution.hpp>
#include <tbb/task_arena.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::exec
{
    namespace detail
    {
        void subscribeRuntimeClose(
            AsyncRuntime& runtime,
            lux::cxx::move_only_function<void(AsyncCloseReport)> completion)
            noexcept
        {
            runtime.subscribeClose(std::move(completion));
        }
    }

    namespace
    {
        using OperationBundleInstallCompletion = lux::cxx::move_only_function<void(
            lux::cxx::expected<std::uint64_t, EAsyncOperationBundleInstallError>)>;
        using OperationBundleCloseCompletion = lux::cxx::move_only_function<void(
            lux::cxx::expected<
                AsyncOperationBundleCloseReport,
                EAsyncOperationBundleCloseError>)>;

        struct RuntimeCloseWaiter final
        {
            explicit RuntimeCloseWaiter(
                lux::cxx::move_only_function<void(AsyncCloseReport)> value)
                noexcept
                : completion(std::move(value))
            {}

            RuntimeCloseWaiter* next{nullptr};
            lux::cxx::move_only_function<void(AsyncCloseReport)> completion;
        };

        [[nodiscard]] RuntimeCloseWaiter* runtimeCloseCompletedSentinel()
            noexcept
        {
            return reinterpret_cast<RuntimeCloseWaiter*>(std::uintptr_t{1u});
        }

        struct ThreadCounts final
        {
            std::size_t blocking_io{2};
            std::size_t background_cpu{2};
        };

        [[nodiscard]] ThreadCounts pickThreadCounts(
            const AsyncRuntimeConfig& config) noexcept
        {
            const auto hw_value = std::thread::hardware_concurrency();
            const std::size_t hw = hw_value == 0u ? 6u : hw_value;
            const std::size_t cpu_default = std::clamp<std::size_t>(
                hw > 3u ? hw - 3u : 1u,
                1u,
                16u);
            return ThreadCounts{
                config.blocking_io_threads == 0u
                    ? 2u
                    : config.blocking_io_threads,
                config.background_cpu_concurrency == 0u
                    ? cpu_default
                    : config.background_cpu_concurrency};
        }
    }

    namespace detail
    {
        class AsyncEndpointRuntimeControl
        {
        public:
            explicit AsyncEndpointRuntimeControl(AsyncRuntime& owner) noexcept
                : owner_(&owner)
            {}
            virtual ~AsyncEndpointRuntimeControl() = default;

            [[nodiscard]] bool postEndpoint(
                std::shared_ptr<AsyncEndpointBase> endpoint) noexcept;
            [[nodiscard]] bool postClosingEndpoint(
                std::shared_ptr<AsyncEndpointBase> endpoint) noexcept;
            [[nodiscard]] bool postSignal(
                std::shared_ptr<CoordinatorSignalState> signal) noexcept;

            [[nodiscard]] bool enterExternal() noexcept
            {
                return enter(external_gate_);
            }
            void leaveExternal() noexcept { leave(external_gate_); }
            [[nodiscard]] bool enterInternal() noexcept
            {
                return enter(internal_gate_);
            }
            void leaveInternal() noexcept { leave(internal_gate_); }
            [[nodiscard]] bool enterMain() noexcept
            {
                return enter(main_gate_);
            }
            void leaveMain() noexcept { leave(main_gate_); }

            void closeExternalAsync(
                lux::cxx::move_only_function<void()> completion) noexcept
            {
                close(external_gate_, std::move(completion));
            }
            void closeInternalAsync(
                lux::cxx::move_only_function<void()> completion) noexcept
            {
                close(internal_gate_, std::move(completion));
            }
            void closeMainAsync(
                lux::cxx::move_only_function<void()> completion) noexcept
            {
                close(main_gate_, std::move(completion));
            }
            void invalidate() noexcept { owner_ = nullptr; }

        protected:
            [[nodiscard]] AsyncRuntime* owner() const noexcept { return owner_; }

        private:
            struct CloseAction final
            {
                explicit CloseAction(
                    lux::cxx::move_only_function<void()> value) noexcept
                    : completion(std::move(value))
                {}

                lux::cxx::move_only_function<void()> completion;
            };

            struct AdmissionGate final
            {
                std::atomic<bool> accepting{true};
                std::atomic<std::uint32_t> active{0u};
                std::atomic<CloseAction*> close_action{nullptr};
            };

            [[nodiscard]] bool enter(AdmissionGate& gate) noexcept
            {
                if (!gate.accepting.load(std::memory_order_acquire) ||
                    owner_ == nullptr)
                    return false;
                gate.active.fetch_add(1u, std::memory_order_acq_rel);
                if (!gate.accepting.load(std::memory_order_acquire) ||
                    owner_ == nullptr)
                {
                    leave(gate);
                    return false;
                }
                return true;
            }

            static void leave(AdmissionGate& gate) noexcept
            {
                if (gate.active.fetch_sub(1u, std::memory_order_acq_rel) == 1u &&
                    !gate.accepting.load(std::memory_order_acquire))
                    completeClose(gate);
            }

            static void close(
                AdmissionGate& gate,
                lux::cxx::move_only_function<void()> completion) noexcept
            {
                auto* action = new (std::nothrow) CloseAction{
                    std::move(completion)};
                if (action == nullptr)
                    std::terminate();
                CloseAction* expected = nullptr;
                if (!gate.close_action.compare_exchange_strong(
                        expected,
                        action,
                        std::memory_order_release,
                        std::memory_order_acquire))
                {
                    delete action;
                    return;
                }
                gate.accepting.store(false, std::memory_order_release);
                if (gate.active.load(std::memory_order_acquire) == 0u)
                    completeClose(gate);
            }

            static void completeClose(AdmissionGate& gate) noexcept
            {
                auto* action = gate.close_action.exchange(
                    nullptr, std::memory_order_acq_rel);
                if (action == nullptr)
                    return;
                auto completion = std::move(action->completion);
                delete action;
                completion();
            }

            AsyncRuntime* owner_{nullptr};
            AdmissionGate external_gate_;
            AdmissionGate internal_gate_;
            AdmissionGate main_gate_;
        };

        class CoordinatorSignalState final
        {
        public:
            CoordinatorSignalState(
                std::weak_ptr<AsyncEndpointRuntimeControl> control,
                lux::cxx::move_only_function<void()> handler) noexcept
                : control_(std::move(control)), handler_(std::move(handler))
            {}

            [[nodiscard]] bool notify(
                const std::shared_ptr<CoordinatorSignalState>& self) noexcept
            {
                if (pending_.exchange(true, std::memory_order_acq_rel))
                    return true;
                auto control = control_.lock();
                if (!control || !control->postSignal(self))
                {
                    pending_.store(false, std::memory_order_release);
                    return false;
                }
                return true;
            }

            void dispatch() noexcept
            {
                pending_.store(false, std::memory_order_release);
                if (handler_)
                    handler_();
            }

        private:
            std::weak_ptr<AsyncEndpointRuntimeControl> control_;
            std::atomic<bool> pending_{false};
            lux::cxx::move_only_function<void()> handler_;
        };

        bool notifyEndpoint(
            const std::weak_ptr<AsyncEndpointRuntimeControl>& weak_control,
            std::shared_ptr<AsyncEndpointBase> endpoint) noexcept
        {
            auto control = weak_control.lock();
            if (!control)
                return false;
            return control->postEndpoint(std::move(endpoint));
        }

        bool notifyClosingEndpoint(
            const std::weak_ptr<AsyncEndpointRuntimeControl>& weak_control,
            std::shared_ptr<AsyncEndpointBase> endpoint) noexcept
        {
            auto control = weak_control.lock();
            if (!control)
                return false;
            return control->postClosingEndpoint(std::move(endpoint));
        }
    }

    struct AsyncRuntime::Impl final
    {
        struct Registration final
        {
            detail::AsyncRegistration descriptor;
            std::shared_ptr<AsyncScope> scope;
            std::uint64_t bundle_id{0u};
        };

        struct OperationBundleState final
        {
            std::shared_ptr<AsyncScope> scope;
            std::vector<std::uint64_t> operation_hashes;
            bool closing{false};
            bool scope_empty{false};
            OperationBundleCloseCompletion close_completion;
        };

        struct TimerEntry final
        {
            TimerEntry(
                asio::io_context& context,
                std::uint64_t value,
                std::chrono::steady_clock::duration delay,
                std::shared_ptr<detail::CoordinatorTimerState> timer_state,
                lux::cxx::move_only_function<void(bool)> callback)
                : timer(context)
                , id(value)
                , state(std::move(timer_state))
                , completion(std::move(callback))
            {
                timer.expires_after(delay);
            }

            asio::steady_timer timer;
            std::uint64_t id{0u};
            std::shared_ptr<detail::CoordinatorTimerState> state;
            lux::cxx::move_only_function<void(bool)> completion;
        };

        using EndpointList = std::vector<
            std::shared_ptr<detail::AsyncEndpointBase>>;
        using WorkGuard = asio::executor_work_guard<
            asio::io_context::executor_type>;

        explicit Impl(
            AsyncRuntime& runtime,
            std::vector<detail::AsyncRegistration>&& registrations,
            AsyncRuntimeConfig cfg)
            : config(std::move(cfg))
            , counts(pickThreadCounts(config))
            , work_guard(asio::make_work_guard(io))
            , blocking_io_pool(
                  static_cast<std::uint32_t>(counts.blocking_io))
            , background_cpu_pool(
                  static_cast<int>(counts.background_cpu),
                  1u,
                  ::tbb::task_arena::priority::low)
            , file_control(detail::makeAsyncFileControl(io, blocking_io_pool))
        {
            owner = &runtime;
            operation_tracker->histograms_enabled =
                config.enable_latency_histograms;
            main.enableLatencyHistograms(
                config.enable_latency_histograms);
            admin_scope = std::make_unique<AsyncScope>(runtime);
            auto endpoints = std::make_shared<EndpointList>();
            endpoints->reserve(registrations.size());
            for (auto& descriptor : registrations)
            {
                auto registration = std::make_unique<Registration>();
                registration->descriptor = std::move(descriptor);
                registration->scope = std::make_shared<AsyncScope>(runtime);
                endpoints->push_back(registration->descriptor.endpoint);
                registry.emplace(
                    registration->descriptor.type.hash,
                    std::move(registration));
            }
            std::atomic_store_explicit(
                &endpoint_snapshot,
                std::const_pointer_cast<const EndpointList>(endpoints),
                std::memory_order_release);
        }

        ~Impl()
        {
            if (coordinator.joinable())
                coordinator.join();
        }

        void attachControl(
            const std::shared_ptr<detail::AsyncEndpointRuntimeControl>& value)
        {
            control = value;
            for (auto& [hash, registration] : registry)
            {
                (void)hash;
                registration->descriptor.endpoint->activate(
                    control,
                    registration->descriptor.module_lease,
                    operation_tracker);
            }
        }

        void start()
        {
            coordinator = std::thread(
                [this]
                {
                    coordinator_id = std::this_thread::get_id();
                    io.run();
                });
        }

        [[nodiscard]] bool hasExactOperation(AsyncTypeToken type) const noexcept
        {
            const auto found = registry.find(type.hash);
            return found != registry.end() &&
                found->second->descriptor.type.name == type.name;
        }

        void publishEndpoint(
            const std::shared_ptr<detail::AsyncEndpointBase>& endpoint)
        {
            auto previous = std::atomic_load_explicit(
                &endpoint_snapshot,
                std::memory_order_acquire);
            auto next = std::make_shared<EndpointList>(
                previous ? *previous : EndpointList{});
            next->push_back(endpoint);
            std::atomic_store_explicit(
                &endpoint_snapshot,
                std::const_pointer_cast<const EndpointList>(next),
                std::memory_order_release);
        }

        void dispatchEndpoint(
            const std::shared_ptr<detail::AsyncEndpointBase>& endpoint) noexcept
        {
            if (!endpoint)
                return;
            wakeups.fetch_add(1u, std::memory_order_relaxed);
            const auto found = registry.find(endpoint->type().hash);
            if (found == registry.end() ||
                found->second->descriptor.type.name != endpoint->type().name)
            {
                endpoint->clearScheduled();
                return;
            }

            auto& registration = *found->second;
            const auto before = endpoint->queued();
            const auto endpoint_state = endpoint->state();
            if (endpoint_state == detail::EAsyncEndpointState::OPEN)
            {
                AsyncOperationContext context{*owner, *registration.scope};
                registration.descriptor.drain(
                    registration.descriptor.handler_state.get(),
                    *endpoint,
                    context,
                    endpoint->drainBatch());
            }
            else
            {
                registration.descriptor.rejectAll(
                    *endpoint,
                    endpoint_state == detail::EAsyncEndpointState::CLOSING
                        ? EAsyncSubmitError::FEATURE_CLOSING
                        : EAsyncSubmitError::STOPPING);
            }
            const auto after = endpoint->queued();
            dispatched.fetch_add(
                before >= after ? before - after : 0u,
                std::memory_order_relaxed);

            if (after != 0u)
            {
                asio::post(
                    io,
                    [this, endpoint]() noexcept
                    {
                        dispatchEndpoint(endpoint);
                    });
                return;
            }

            endpoint->clearScheduled();
            if (endpoint->queued() != 0u && endpoint->markScheduled())
            {
                asio::post(
                    io,
                    [this, endpoint]() noexcept
                    {
                        dispatchEndpoint(endpoint);
                    });
                return;
            }

            if (registration.bundle_id != 0u)
                tryFinalizeOperationBundleClose(registration.bundle_id);
            if (global_closing)
                tryFinishGlobalAdmissionClose();
        }

        void scheduleClosingEndpoint(
            const std::shared_ptr<detail::AsyncEndpointBase>& endpoint) noexcept
        {
            (void)detail::notifyClosingEndpoint(
                endpoint->runtimeControl(), endpoint);
        }

        void tryFinalizeOperationBundleClose(std::uint64_t bundle_id) noexcept
        {
            const auto current = operation_bundles.find(bundle_id);
            if (current == operation_bundles.end() || !current->second.closing ||
                !current->second.scope_empty)
                return;

            for (const auto hash : current->second.operation_hashes)
            {
                const auto registration = registry.find(hash);
                if (registration == registry.end())
                    continue;
                const auto& endpoint = registration->second->descriptor.endpoint;
                if (endpoint->activeProducers() != 0u ||
                    endpoint->queued() != 0u || endpoint->isScheduled())
                    return;
            }

            AsyncOperationBundleCloseReport report;
            report.removed_operations = current->second.operation_hashes.size();
            auto completion = std::move(current->second.close_completion);
            auto scope = current->second.scope;
            for (const auto hash : current->second.operation_hashes)
            {
                const auto registration = registry.find(hash);
                if (registration == registry.end())
                    continue;
                if (!registration->second->descriptor.endpoint->deactivate())
                    return;
                registry.erase(registration);
            }
            operation_bundles.erase(current);
            if (completion)
                completion(report);
        }

        void tryFinishGlobalAdmissionClose() noexcept
        {
            if (!global_closing || close_started.load(std::memory_order_acquire))
                return;
            for (const auto& [hash, registration] : registry)
            {
                (void)hash;
                const auto& endpoint = registration->descriptor.endpoint;
                if (endpoint->activeProducers() != 0u ||
                    endpoint->queued() != 0u)
                    return;
            }
            bool expected = false;
            if (!close_started.compare_exchange_strong(
                    expected, true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
                return;

            pending_scope_closes.store(
                shutdown_scopes.size() + 1u,
                std::memory_order_release);
            auto start_close = [this](AsyncScope& scope) noexcept
            {
                auto close = scope.closeAsync()
                    | stdexec::continues_on(CoordinatorScheduler{owner->client()})
                    | stdexec::then(
                          [this]() noexcept
                          {
                              finishScopeClose();
                          });
                ::experimental::execution::start_detached(std::move(close));
            };
            for (auto& scope : shutdown_scopes)
                start_close(*scope);
            start_close(*admin_scope);
        }

        void finishScopeClose() noexcept
        {
            if (pending_scope_closes.fetch_sub(
                    1u, std::memory_order_acq_rel) != 1u)
                return;

            control->closeMainAsync(
                [this]() noexcept
                {
                    asio::post(
                        io,
                        [this]() noexcept
                        {
                            control->closeInternalAsync(
                                [this]() noexcept
                                {
                                    work_guard.reset();
                                    completeClose();
                                });
                        });
                });
        }

        void completeClose() noexcept
        {
            closed.store(true, std::memory_order_release);
            final_close_report = snapshot(EAsyncCloseStatus::CLOSED);
            auto* waiter = close_waiters.exchange(
                runtimeCloseCompletedSentinel(),
                std::memory_order_acq_rel);
            while (waiter != nullptr &&
                   waiter != runtimeCloseCompletedSentinel())
            {
                auto* next = waiter->next;
                auto completion = std::move(waiter->completion);
                delete waiter;
                completion(final_close_report);
                waiter = next;
            }
        }

        void subscribeClose(
            lux::cxx::move_only_function<void(AsyncCloseReport)> completion)
            noexcept
        {
            auto* waiter = new (std::nothrow) RuntimeCloseWaiter{
                std::move(completion)};
            if (waiter == nullptr)
                std::terminate();

            auto* head = close_waiters.load(std::memory_order_acquire);
            for (;;)
            {
                if (head == runtimeCloseCompletedSentinel())
                {
                    auto terminal = std::move(waiter->completion);
                    delete waiter;
                    terminal(final_close_report);
                    return;
                }
                waiter->next = head;
                if (close_waiters.compare_exchange_weak(
                        head,
                        waiter,
                        std::memory_order_release,
                        std::memory_order_acquire))
                    break;
            }

            if (close_requested.exchange(true, std::memory_order_acq_rel))
                return;
            control->closeExternalAsync(
                [this]() noexcept
                {
                    asio::post(
                        io,
                        [this]() noexcept
                        {
                            beginClose();
                        });
                });
        }

        void installOperations(
            AsyncOperationBundle&& package,
            OperationBundleInstallCompletion completion) noexcept
        {
            if (global_closing)
            {
                completion(lux::cxx::unexpected(EAsyncOperationBundleInstallError::STOPPING));
                return;
            }
            auto descriptors = detailTakeOperationRegistrations(std::move(package));
            if (descriptors.empty())
            {
                completion(lux::cxx::unexpected(
                    EAsyncOperationBundleInstallError::INVALID_PACKAGE));
                return;
            }
            for (const auto& descriptor : descriptors)
            {
                const auto live = registry.find(descriptor.type.hash);
                if (live != registry.end())
                {
                    completion(lux::cxx::unexpected(
                        live->second->descriptor.type.name == descriptor.type.name
                            ? EAsyncOperationBundleInstallError::DUPLICATE_OPERATION
                            : EAsyncOperationBundleInstallError::TYPE_COLLISION));
                    return;
                }
                for (const auto& dependency : descriptor.prerequisites)
                {
                    bool found = hasExactOperation(dependency);
                    if (!found)
                    {
                        found = std::ranges::any_of(
                            descriptors,
                            [&](const auto& candidate)
                            {
                                return candidate.type == dependency;
                            });
                    }
                    if (!found)
                    {
                        completion(lux::cxx::unexpected(
                            EAsyncOperationBundleInstallError::MISSING_DEPENDENCY));
                        return;
                    }
                }
            }

            const auto bundle_id = next_operation_bundle_id++;
            auto scope = std::make_shared<AsyncScope>(*owner);
            OperationBundleState bundle;
            bundle.scope = scope;
            bundle.operation_hashes.reserve(descriptors.size());
            for (auto& descriptor : descriptors)
            {
                const auto hash = descriptor.type.hash;
                auto registration = std::make_unique<Registration>();
                registration->descriptor = std::move(descriptor);
                registration->scope = scope;
                registration->bundle_id = bundle_id;
                registration->descriptor.endpoint->activate(
                    control,
                    registration->descriptor.module_lease,
                    operation_tracker);
                publishEndpoint(registration->descriptor.endpoint);
                bundle.operation_hashes.push_back(hash);
                registry.emplace(hash, std::move(registration));
            }
            operation_bundles.emplace(bundle_id, std::move(bundle));
            completion(bundle_id);
        }

        void closeOperations(
            std::uint64_t bundle_id,
            OperationBundleCloseCompletion completion) noexcept
        {
            if (global_closing)
            {
                completion(lux::cxx::unexpected(EAsyncOperationBundleCloseError::STOPPING));
                return;
            }
            const auto found = operation_bundles.find(bundle_id);
            if (found == operation_bundles.end())
            {
                completion(lux::cxx::unexpected(
                    EAsyncOperationBundleCloseError::UNKNOWN_BUNDLE));
                return;
            }
            if (found->second.closing)
            {
                completion(lux::cxx::unexpected(
                    EAsyncOperationBundleCloseError::CLOSE_IN_PROGRESS));
                return;
            }

            found->second.closing = true;
            found->second.close_completion = std::move(completion);
            for (const auto hash : found->second.operation_hashes)
            {
                const auto registration = registry.find(hash);
                if (registration == registry.end())
                    continue;
                auto& descriptor = registration->second->descriptor;
                descriptor.endpoint->closeAdmission(false);
                scheduleClosingEndpoint(descriptor.endpoint);
            }
            auto scope = found->second.scope;
            auto finalize = scope->closeAsync()
                | stdexec::continues_on(CoordinatorScheduler{owner->client()})
                | stdexec::then(
                      [this, bundle_id, scope]() mutable noexcept
                      {
                          const auto current = operation_bundles.find(bundle_id);
                          if (current == operation_bundles.end())
                              return;
                          current->second.scope_empty = true;
                          tryFinalizeOperationBundleClose(bundle_id);
                      });
            if (!spawn(*admin_scope, std::move(finalize)))
            {
                auto failed = std::move(found->second.close_completion);
                found->second.closing = false;
                if (failed)
                    failed(lux::cxx::unexpected(EAsyncOperationBundleCloseError::STOPPING));
            }
        }

        void scheduleTimer(
            std::chrono::steady_clock::duration delay,
            std::shared_ptr<detail::CoordinatorTimerState> state,
            lux::cxx::move_only_function<void(bool)> completion) noexcept
        {
            if (global_closing ||
                state->cancelled.load(std::memory_order_acquire))
            {
                completion(true);
                return;
            }
            const auto id = next_timer_id++;
            auto entry = std::make_shared<TimerEntry>(
                io, id, delay, state, std::move(completion));
            state->id.store(id, std::memory_order_release);
            timers.emplace(id, entry);
            pending_timers.fetch_add(1u, std::memory_order_relaxed);
            entry->timer.async_wait(
                [this, id](const std::error_code& error) noexcept
                {
                    finishTimer(
                        id,
                        error == asio::error::operation_aborted);
                });
        }

        void finishTimer(std::uint64_t id, bool stopped) noexcept
        {
            const auto found = timers.find(id);
            if (found == timers.end())
                return;
            auto completion = std::move(found->second->completion);
            found->second->state->id.store(0u, std::memory_order_release);
            timers.erase(found);
            pending_timers.fetch_sub(1u, std::memory_order_release);
            completion(stopped);
        }

        void beginClose() noexcept
        {
            global_closing = true;
            file_control->closeAdmissionAndCancel();
            shutdown_scopes.clear();
            for (auto& [hash, registration] : registry)
            {
                (void)hash;
                auto& descriptor = registration->descriptor;
                descriptor.endpoint->closeAdmission(true);
                scheduleClosingEndpoint(descriptor.endpoint);
                registration->scope->requestStop();
                if (std::ranges::find(
                        shutdown_scopes,
                        registration->scope) == shutdown_scopes.end())
                    shutdown_scopes.push_back(registration->scope);
            }
            for (auto& [id, timer] : timers)
            {
                (void)id;
                timer->timer.cancel();
            }
            tryFinishGlobalAdmissionClose();
        }

        [[nodiscard]] AsyncCloseReport snapshot(
            EAsyncCloseStatus status) const noexcept
        {
            std::size_t queued = 0u;
            std::size_t bytes = 0u;
            std::size_t queue_high = 0u;
            std::size_t byte_high = 0u;
            std::size_t scheduled_queued_endpoints = 0u;
            std::size_t unscheduled_queued_endpoints = 0u;
            const auto endpoints = std::atomic_load_explicit(
                &endpoint_snapshot,
                std::memory_order_acquire);
            if (endpoints)
            {
                for (const auto& endpoint : *endpoints)
                {
                    const auto endpoint_queued = endpoint->queued();
                    queued += endpoint_queued;
                    if (endpoint_queued != 0u)
                    {
                        if (endpoint->isScheduled())
                            ++scheduled_queued_endpoints;
                        else
                            ++unscheduled_queued_endpoints;
                    }
                    bytes += endpoint->queuedBytes();
                    queue_high += endpoint->queueHighWater();
                    byte_high += endpoint->byteHighWater();
                }
            }
            const auto tracked = operation_tracker->tracked.load(
                std::memory_order_relaxed);
            const auto main_stats = main.statistics();
            const auto file_stats = file_control->statistics();
            AsyncCloseReport report;
            report.status = status;
            report.accepted = tracked;
            report.dispatched = dispatched.load(std::memory_order_relaxed);
            report.rejected =
                operation_tracker->runtime_failed.load(
                    std::memory_order_relaxed) +
                operation_tracker->stopped.load(
                    std::memory_order_relaxed);
            report.tracked_operations = tracked;
            report.succeeded = operation_tracker->succeeded.load(
                std::memory_order_relaxed);
            report.domain_failed = operation_tracker->domain_failed.load(
                std::memory_order_relaxed);
            report.runtime_failed = operation_tracker->runtime_failed.load(
                std::memory_order_relaxed);
            report.stopped = operation_tracker->stopped.load(
                std::memory_order_relaxed);
            report.wakeups = wakeups.load(std::memory_order_relaxed);
            report.active_operations = operation_tracker->active.load(
                std::memory_order_acquire);
            report.queued_packets = queued;
            report.scheduled_queued_endpoints = scheduled_queued_endpoints;
            report.unscheduled_queued_endpoints = unscheduled_queued_endpoints;
            report.running_operations =
                report.active_operations > report.queued_packets
                    ? report.active_operations - report.queued_packets
                    : 0u;
            report.queued_bytes = bytes;
            report.queue_high_water = queue_high;
            report.byte_high_water = byte_high;
            report.pending_timers = pending_timers.load(
                std::memory_order_acquire);
            report.asio_file_pending =
                file_stats.active_pending_operations;
            report.asio_native_file_requests =
                file_stats.active_native_requests;
            report.blocking_io_running = file_stats.blocking_io_running +
                blocking_io_activity->load(std::memory_order_acquire);
            report.background_cpu_running =
                background_cpu_activity->load(std::memory_order_acquire);
            report.file_request_state_allocations =
                file_stats.request_state_allocations;
            report.file_pending_state_allocations =
                file_stats.pending_state_allocations;
            report.file_native_state_allocations =
                file_stats.native_state_allocations;
            report.main_completion_pending = main_stats.depth != 0u;
            report.main_queue_depth = main_stats.depth;
            report.main_queue_high_water = main_stats.high_water;
            report.main_oldest_age_ns = main_stats.oldest_age_ns;
            report.main_adoption_samples = main_stats.adoption_samples;
            report.main_adoption_total_ns = main_stats.adoption_total_ns;
            report.main_adoption_max_ns = main_stats.adoption_max_ns;
            report.endpoint_queue_wait_samples =
                operation_tracker->queue_wait_samples.load(
                    std::memory_order_relaxed);
            report.endpoint_queue_wait_total_ns =
                operation_tracker->queue_wait_total_ns.load(
                    std::memory_order_relaxed);
            report.endpoint_queue_wait_max_ns =
                operation_tracker->queue_wait_max_ns.load(
                    std::memory_order_relaxed);
            report.coordinator_handler_samples =
                operation_tracker->handler_samples.load(
                    std::memory_order_relaxed);
            report.coordinator_handler_total_ns =
                operation_tracker->handler_total_ns.load(
                    std::memory_order_relaxed);
            report.coordinator_handler_max_ns =
                operation_tracker->handler_max_ns.load(
                    std::memory_order_relaxed);
            report.main_adoption_histogram =
                main_stats.adoption_histogram;
            for (std::size_t index = 0u;
                 index < kAsyncLatencyBucketCount;
                 ++index)
            {
                report.endpoint_queue_wait_histogram[index] =
                    operation_tracker->queue_wait_histogram[index].load(
                        std::memory_order_relaxed);
                report.coordinator_handler_histogram[index] =
                    operation_tracker->handler_histogram[index].load(
                        std::memory_order_relaxed);
            }
            return report;
        }

        AsyncRuntimeConfig config;
        AsyncRuntime* owner{nullptr};
        ThreadCounts counts;
        asio::io_context io;
        std::optional<WorkGuard> work_guard;
        ::exec::static_thread_pool blocking_io_pool;
        ::experimental::execution::tbb::tbb_thread_pool background_cpu_pool;
        std::shared_ptr<detail::AsyncFileControl> file_control;
        MainThreadMailbox main;
        std::thread coordinator;
        std::thread::id coordinator_id{};
        std::shared_ptr<detail::AsyncEndpointRuntimeControl> control;
        std::unordered_map<std::uint64_t, std::unique_ptr<Registration>> registry;
        std::unordered_map<std::uint64_t, OperationBundleState> operation_bundles;
        std::unique_ptr<AsyncScope> admin_scope;
        std::vector<std::shared_ptr<AsyncScope>> shutdown_scopes;
        std::unordered_map<std::uint64_t, std::shared_ptr<TimerEntry>> timers;
        // C++20's shared_ptr atomic free functions are supported by the
        // Android libc++ versions used by the project even where the optional
        // atomic<shared_ptr<T>> class-template specialization is absent.
        std::shared_ptr<const EndpointList> endpoint_snapshot;
        std::shared_ptr<detail::AsyncOperationTracker> operation_tracker{
            std::make_shared<detail::AsyncOperationTracker>()};
        std::shared_ptr<std::atomic<std::size_t>> blocking_io_activity{
            std::make_shared<std::atomic<std::size_t>>(0u)};
        std::shared_ptr<std::atomic<std::size_t>> background_cpu_activity{
            std::make_shared<std::atomic<std::size_t>>(0u)};
        std::uint64_t next_operation_bundle_id{1u};
        std::uint64_t next_timer_id{1u};
        std::atomic<std::uint64_t> dispatched{0u};
        std::atomic<std::uint64_t> wakeups{0u};
        std::atomic<std::size_t> pending_timers{0u};
        std::atomic<bool> close_started{false};
        std::atomic<std::size_t> pending_scope_closes{0u};
        std::atomic<RuntimeCloseWaiter*> close_waiters{nullptr};
        std::atomic<bool> close_requested{false};
        std::atomic<bool> closed{false};
        std::atomic<bool> joined{false};
        AsyncCloseReport final_close_report{};
        bool global_closing{false};
    };

    class AsyncClientControl final :
        public detail::AsyncEndpointRuntimeControl
    {
    public:
        explicit AsyncClientControl(AsyncRuntime& owner) noexcept
            : detail::AsyncEndpointRuntimeControl(owner)
        {}

        [[nodiscard]] bool postContinuation(
            lux::cxx::move_only_function<void()> task) noexcept
        {
            if (!task || !enterInternal())
                return false;
            auto* runtime = owner();
            asio::post(
                runtime->impl_->io,
                [this, task = std::move(task)]() mutable noexcept
                {
                    task();
                    leaveInternal();
                });
            return true;
        }

        [[nodiscard]] bool scheduleAfter(
            std::chrono::steady_clock::duration delay,
            std::shared_ptr<detail::CoordinatorTimerState> state,
            lux::cxx::move_only_function<void(bool)> completion) noexcept
        {
            if (!state || !completion || !enterInternal())
                return false;
            auto* runtime = owner();
            asio::post(
                runtime->impl_->io,
                [this, runtime, delay, state = std::move(state),
                 completion = std::move(completion)]() mutable noexcept
                {
                    runtime->impl_->scheduleTimer(
                        delay,
                        std::move(state),
                        [this, completion = std::move(completion)](
                            bool stopped) mutable noexcept
                        {
                            completion(stopped);
                            leaveInternal();
                        });
                });
            return true;
        }

        void cancelTimer(
            const std::shared_ptr<detail::CoordinatorTimerState>& state)
            noexcept
        {
            if (!state)
                return;
            state->cancelled.store(true, std::memory_order_release);
            if (!enterInternal())
                return;
            auto* runtime = owner();
            asio::post(
                runtime->impl_->io,
                [this, runtime, state]() noexcept
                {
                    const auto id = state->id.load(std::memory_order_acquire);
                    if (id != 0u)
                    {
                        const auto found = runtime->impl_->timers.find(id);
                        if (found != runtime->impl_->timers.end())
                            found->second->timer.cancel();
                    }
                    leaveInternal();
                });
        }

        [[nodiscard]] bool installOperations(
            AsyncOperationBundle&& package,
            OperationBundleInstallCompletion completion) noexcept
        {
            if (!enterExternal())
            {
                completion(lux::cxx::unexpected(
                    EAsyncOperationBundleInstallError::STOPPING));
                return false;
            }
            auto* runtime = owner();
            asio::post(
                runtime->impl_->io,
                [this, runtime, package = std::move(package),
                 completion = std::move(completion)]() mutable noexcept
                {
                    runtime->impl_->installOperations(
                        std::move(package), std::move(completion));
                    leaveExternal();
                });
            return true;
        }

        [[nodiscard]] bool closeOperations(
            std::uint64_t bundle_id,
            OperationBundleCloseCompletion completion) noexcept
        {
            if (!enterExternal())
            {
                completion(lux::cxx::unexpected(EAsyncOperationBundleCloseError::STOPPING));
                return false;
            }
            auto* runtime = owner();
            asio::post(
                runtime->impl_->io,
                [this, runtime, bundle_id,
                 completion = std::move(completion)]() mutable noexcept
                {
                    runtime->impl_->closeOperations(
                        bundle_id, std::move(completion));
                    leaveExternal();
                });
            return true;
        }

        [[nodiscard]] bool tryDispatchToMainThread(
            lux::cxx::move_only_function<void()> task) noexcept
        {
            if (!task || !enterMain())
                return false;
            auto* runtime = owner();
            runtime->impl_->main.enqueue(
                [this, task = std::move(task)]() mutable noexcept
                {
                    task();
                    leaveMain();
                });
            return true;
        }
    };

    bool detail::AsyncEndpointRuntimeControl::postEndpoint(
        std::shared_ptr<AsyncEndpointBase> endpoint) noexcept
    {
        if (!endpoint || !enterExternal())
            return false;
        if (!endpoint->markScheduled())
        {
            leaveExternal();
            return true;
        }
        auto* runtime = owner();
        asio::post(
            runtime->impl_->io,
            [this, runtime, endpoint = std::move(endpoint)]() noexcept
            {
                runtime->impl_->dispatchEndpoint(endpoint);
                leaveExternal();
            });
        return true;
    }

    bool detail::AsyncEndpointRuntimeControl::postClosingEndpoint(
        std::shared_ptr<AsyncEndpointBase> endpoint) noexcept
    {
        if (!endpoint || !enterInternal())
            return false;
        if (!endpoint->markScheduled())
        {
            leaveInternal();
            return true;
        }
        auto* runtime = owner();
        asio::post(
            runtime->impl_->io,
            [this, runtime, endpoint = std::move(endpoint)]() noexcept
            {
                runtime->impl_->dispatchEndpoint(endpoint);
                leaveInternal();
            });
        return true;
    }

    bool detail::AsyncEndpointRuntimeControl::postSignal(
        std::shared_ptr<CoordinatorSignalState> signal) noexcept
    {
        if (!signal || !enterExternal())
            return false;
        auto* runtime = owner();
        asio::post(
            runtime->impl_->io,
            [this, signal = std::move(signal)]() noexcept
            {
                signal->dispatch();
                leaveExternal();
            });
        return true;
    }

    bool AsyncClient::tryDispatchToMainThreadContinuation(
        lux::cxx::move_only_function<void()> task) const noexcept
    {
        auto control = control_.lock();
        return control && control->postContinuation(std::move(task));
    }

    bool AsyncClient::tryScheduleAfter(
        std::chrono::steady_clock::duration delay,
        std::shared_ptr<detail::CoordinatorTimerState> state,
        lux::cxx::move_only_function<void(bool)> completion) const noexcept
    {
        auto control = control_.lock();
        return control && control->scheduleAfter(
            delay, std::move(state), std::move(completion));
    }

    void AsyncClient::cancelScheduledTimer(
        const std::shared_ptr<detail::CoordinatorTimerState>& state) const
        noexcept
    {
        if (auto control = control_.lock())
            control->cancelTimer(state);
        else if (state)
            state->cancelled.store(true, std::memory_order_release);
    }

    bool AsyncClient::tryInstallOperations(
        AsyncOperationBundle&& package,
        OperationBundleInstallCompletion completion) const noexcept
    {
        auto control = control_.lock();
        if (!control)
        {
            completion(lux::cxx::unexpected(EAsyncOperationBundleInstallError::STOPPING));
            return false;
        }
        return control->installOperations(
            std::move(package), std::move(completion));
    }

    bool AsyncClient::tryCloseOperations(
        std::uint64_t bundle_id,
        OperationBundleCloseCompletion completion) const noexcept
    {
        auto control = control_.lock();
        if (!control)
        {
            completion(lux::cxx::unexpected(EAsyncOperationBundleCloseError::STOPPING));
            return false;
        }
        return control->closeOperations(bundle_id, std::move(completion));
    }

    void AsyncClient::closeOperationsDetached(std::uint64_t bundle_id) const noexcept
    {
        (void)tryCloseOperations(
            bundle_id,
            [](lux::cxx::expected<
                AsyncOperationBundleCloseReport,
                EAsyncOperationBundleCloseError>) noexcept {});
    }

    bool MainThreadDispatcher::tryDispatchToMainThread(
        lux::cxx::move_only_function<void()> task) const noexcept
    {
        auto control = control_.lock();
        return control && control->tryDispatchToMainThread(std::move(task));
    }

    bool CoordinatorSignal::notify() const noexcept
    {
        return state_ && state_->notify(state_);
    }

    MainThreadDispatcher AsyncOperationContext::mainThreadDispatcher() const noexcept
    {
        return runtime_->mainThreadDispatcher();
    }

    AsyncRuntime::AsyncRuntime(AsyncRuntimeConfig config)
        : AsyncRuntime(AsyncRuntimePlan{}, std::move(config))
    {}

    AsyncRuntime::AsyncRuntime(
        AsyncRuntimePlan&& plan,
        AsyncRuntimeConfig config)
        : impl_(std::make_unique<Impl>(
              *this,
              std::move(plan.registrations_),
              std::move(config)))
        , client_control_(std::make_shared<AsyncClientControl>(*this))
        , file_service_(std::unique_ptr<AsyncFileService>(
              new AsyncFileService(impl_->file_control)))
    {
        impl_->attachControl(client_control_);
        impl_->start();
    }

    AsyncRuntime::~AsyncRuntime()
    {
        if (!impl_->joined.load(std::memory_order_acquire))
            std::terminate();
    }

    AsyncClient AsyncRuntime::client() noexcept
    {
        return AsyncClient{client_control_};
    }

    MainThreadDispatcher AsyncRuntime::mainThreadDispatcher() noexcept
    {
        return MainThreadDispatcher{client_control_};
    }

    CoordinatorSignal AsyncRuntime::makeCoordinatorSignal(
        lux::cxx::move_only_function<void()> handler) noexcept
    {
        if (!handler || impl_->closed.load(std::memory_order_acquire))
            return {};
        return CoordinatorSignal{
            std::make_shared<detail::CoordinatorSignalState>(
                client_control_, std::move(handler))};
    }

    AsyncFileService& AsyncRuntime::fileService() noexcept
    {
        return *file_service_;
    }

    std::size_t AsyncRuntime::drainMainThreadCompletions(std::size_t budget)
    {
        if (budget == static_cast<std::size_t>(-1))
            budget = impl_->config.main_thread_drain_budget;
        return impl_->main.pump(budget);
    }

    bool AsyncRuntime::isDrainingMainThreadCompletions() const noexcept
    {
        return impl_->main.isPumping();
    }

    AsyncRuntimeStats AsyncRuntime::stats() const noexcept
    {
        const auto report = impl_->snapshot(
            impl_->closed.load(std::memory_order_acquire)
                ? EAsyncCloseStatus::CLOSED
                : EAsyncCloseStatus::CLOSE_IN_PROGRESS);
        return static_cast<const AsyncRuntimeStats&>(report);
    }

    bool AsyncRuntime::latencyHistogramsEnabled() const noexcept
    {
        return impl_->config.enable_latency_histograms;
    }

    AsyncOperationBundleInstallSender AsyncRuntime::installOperations(
        AsyncOperationBundle package) noexcept
    {
        return AsyncOperationBundleInstallSender{client(), std::move(package)};
    }

    AsyncOperationBundleCloseSender AsyncRuntime::closeOperations(
        AsyncOperationBundleLease&& lease) noexcept
    {
        return AsyncOperationBundleCloseSender{std::move(lease)};
    }

    AsyncRuntimeCloseSender AsyncRuntime::closeAsync() noexcept
    {
        return AsyncRuntimeCloseSender{*this};
    }

    void AsyncRuntime::subscribeClose(
        lux::cxx::move_only_function<void(AsyncCloseReport)> completion)
        noexcept
    {
        impl_->subscribeClose(std::move(completion));
    }

    lux::cxx::expected<void, EAsyncJoinError> AsyncRuntime::join() noexcept
    {
        if (impl_->coordinator_id == std::this_thread::get_id())
            return lux::cxx::unexpected(EAsyncJoinError::WRONG_THREAD);
        if (!impl_->closed.load(std::memory_order_acquire))
            return lux::cxx::unexpected(
                EAsyncJoinError::CLOSE_NOT_COMPLETE);
        if (impl_->joined.exchange(true, std::memory_order_acq_rel))
            return lux::cxx::unexpected(EAsyncJoinError::ALREADY_JOINED);

        if (impl_->coordinator.joinable())
            impl_->coordinator.join();
        impl_->blocking_io_pool.request_stop();
        client_control_->invalidate();
        return {};
    }

    ::experimental::execution::static_thread_pool&
    AsyncRuntime::blockingIoPool() noexcept
    {
        return impl_->blocking_io_pool;
    }

    ::experimental::execution::tbb::tbb_thread_pool&
    AsyncRuntime::backgroundCpuPool() noexcept
    {
        return impl_->background_cpu_pool;
    }

    std::shared_ptr<std::atomic<std::size_t>>
    AsyncRuntime::blockingIoActivityCounter() noexcept
    {
        return impl_->blocking_io_activity;
    }

    std::shared_ptr<std::atomic<std::size_t>>
    AsyncRuntime::backgroundCpuActivityCounter() noexcept
    {
        return impl_->background_cpu_activity;
    }

    MainThreadMailbox& AsyncRuntime::mainThreadMailbox() noexcept
    {
        return impl_->main;
    }
}
