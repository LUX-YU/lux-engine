#pragma once

#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>
#include <lux/engine/runtime/logging/LogRouter.hpp>

#include <atomic>
#include <optional>

namespace lux::logging::testing
{
    [[nodiscard]] inline LogRouterStatistics closeLogRouter(
        LogRouter& router,
        lux::exec::AsyncRuntime& runtime) noexcept
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        std::atomic<bool> closed{false};
        std::optional<LogRouterStatistics> statistics;
        detail::subscribeLogRouterClose(
            router,
            [&closed, &progress, &statistics](LogRouterStatistics value)
                noexcept
            {
                statistics.emplace(std::move(value));
                closed.store(true, std::memory_order_release);
                progress.notify();
            }
        );
        progress.drive(
            [&closed]() noexcept
            {
                return closed.load(std::memory_order_acquire);
            }
        );
        return std::move(*statistics);
    }
}
