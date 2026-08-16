#pragma once

// Build-tree-only deterministic close driver for tests that do not assemble a
// product-level safe-point driver.

#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/detail/MainThreadMailbox.hpp>

#include <exec/start_detached.hpp>

#include <atomic>
#include <memory>
#include <optional>

namespace lux::exec::testing
{
    class CloseEpoch final
    {
    public:
        explicit CloseEpoch(AsyncRuntime& runtime)
            : runtime_(runtime)
            , epoch_(std::make_shared<std::atomic<std::uint64_t>>(0u))
            , binding_(std::make_shared<const MainThreadMailbox::WakeBinding>(
                  MainThreadMailbox::WakeBinding{
                      epoch_,
                      [](void* opaque) noexcept
                      {
                          auto& epoch = *static_cast<
                              std::atomic<std::uint64_t>*>(opaque);
                          epoch.fetch_add(1u, std::memory_order_release);
                          epoch.notify_one();
                      }}))
        {
            runtime_.mainThreadMailbox().bindExternalWake(binding_);
        }

        ~CloseEpoch()
        {
            runtime_.mainThreadMailbox().unbindExternalWake(binding_);
        }

        void notify() noexcept
        {
            epoch_->fetch_add(1u, std::memory_order_release);
            epoch_->notify_one();
        }

        template <class Predicate>
        void drive(Predicate&& done) noexcept
        {
            while (!done())
            {
                const auto observed = epoch_->load(std::memory_order_acquire);
                (void)runtime_.drainMainThreadCompletions(256u);
                if (!done())
                    epoch_->wait(observed, std::memory_order_acquire);
            }
        }

        /// Drive an owner safe point together with mailbox adoption. This is
        /// the test-only counterpart of MainCloseDriver for fixtures whose
        /// completion also requires an ECS command barrier. Waiting is tied
        /// to the same progress epoch as AsyncRuntime; it never spins,
        /// sleeps, or relies on a fixed pump count.
        template <class Step, class Predicate, class LocalWork>
        void driveWithStep(
            Step&& step,
            Predicate&& done,
            LocalWork&& has_local_work) noexcept
        {
            while (!done())
            {
                const auto observed = epoch_->load(
                    std::memory_order_acquire);
                (void)runtime_.drainMainThreadCompletions(256u);
                step();
                if (!done() && !has_local_work() &&
                    epoch_->load(std::memory_order_acquire) == observed)
                {
                    epoch_->wait(observed, std::memory_order_acquire);
                }
            }
        }

    private:
        AsyncRuntime& runtime_;
        std::shared_ptr<std::atomic<std::uint64_t>> epoch_;
        std::shared_ptr<const MainThreadMailbox::WakeBinding> binding_;
    };

    [[nodiscard]] inline AsyncCloseReport closeRuntime(
        AsyncRuntime& runtime) noexcept
    {
        CloseEpoch progress{runtime};
        std::atomic<bool> closed{false};
        std::optional<AsyncCloseReport> report;
        detail::subscribeRuntimeClose(
            runtime,
            [&closed, &progress, &report](AsyncCloseReport value) noexcept
            {
                report.emplace(std::move(value));
                closed.store(true, std::memory_order_release);
                progress.notify();
            });
        progress.drive(
            [&closed]() noexcept
            {
                return closed.load(std::memory_order_acquire);
            });
        (void)runtime.join();
        return std::move(*report);
    }

    [[nodiscard]] inline lux::cxx::expected<
        AsyncOperationBundleCloseReport,
        EAsyncOperationBundleCloseError>
    closeOperations(
        AsyncRuntime& runtime,
        AsyncOperationBundleLease&& lease) noexcept
    {
        CloseEpoch progress{runtime};
        std::atomic<bool> closed{false};
        std::optional<lux::cxx::expected<
            AsyncOperationBundleCloseReport,
            EAsyncOperationBundleCloseError>> result;
        auto close = runtime.closeOperations(std::move(lease))
            | stdexec::then(
                  [&closed, &progress, &result](auto value) noexcept
                  {
                      result.emplace(std::move(value));
                      closed.store(true, std::memory_order_release);
                      progress.notify();
                  });
        ::experimental::execution::start_detached(std::move(close));
        progress.drive(
            [&closed]() noexcept
            {
                return closed.load(std::memory_order_acquire);
            });
        return std::move(*result);
    }

    inline void closeScope(AsyncScope& scope, AsyncRuntime& runtime) noexcept
    {
        CloseEpoch progress{runtime};
        std::atomic<bool> closed{false};
        detail::subscribeScopeClose(
            scope,
            [&closed, &progress]() noexcept
            {
                closed.store(true, std::memory_order_release);
                progress.notify();
            });
        progress.drive(
            [&closed]() noexcept
            {
                return closed.load(std::memory_order_acquire);
            });
    }

}
