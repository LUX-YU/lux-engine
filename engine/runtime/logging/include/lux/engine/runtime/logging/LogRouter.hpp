#pragma once
/**
 * @file LogRouter.hpp
 * @brief Process-domain asynchronous diagnostic routing.
 *
 * Logging threads only copy a LogRecord into a bounded MPMC queue.  The
 * AsyncRuntime coordinator coalesces drain intent and the IO pool performs
 * formatting/output.  DomainEvents is deliberately not part of this path.
 */

#include <lux/engine/log/LogRecord.hpp>
#include <lux/cxx/core/move_only_function.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::exec
{
    class AsyncRuntime;
}

namespace lux::logging
{
    class LogRouterCloseSender;
    class LogRouter;

    struct LogRouterConfig final
    {
        std::size_t queue_capacity{8192};
        std::size_t batch_size{256};
    };

    struct LogRouterStatistics final
    {
        std::uint64_t accepted{0};
        std::uint64_t written{0};
        std::uint64_t dropped{0};
        std::uint64_t dropped_toasts{0};
        std::uint64_t flush_latency_ns{0};
        std::size_t   queue_high_water{0};
    };

    namespace detail
    {
        void subscribeLogRouterClose(
            LogRouter& router,
            lux::cxx::move_only_function<void(LogRouterStatistics)>
                completion) noexcept;
    }

    class LogRouter final
    {
    public:
        using ToastSink =
            lux::cxx::move_only_function<void(const lux::log::LogRecord&)>;

        explicit LogRouter(
            lux::exec::AsyncRuntime& runtime,
            LogRouterConfig config = {});
        ~LogRouter();

        LogRouter(const LogRouter&) = delete;
        LogRouter& operator=(const LogRouter&) = delete;
        LogRouter(LogRouter&&) = delete;
        LogRouter& operator=(LogRouter&&) = delete;

        /// Installs the sole lux::log output seam. Call once during host
        /// assembly, after the runtime exists.
        void install(ToastSink toast = {});

        /// Flush accepted records, detach the global outlet and switch late
        /// logs back to synchronous stderr. Idempotent; main thread only.
        [[nodiscard]] LogRouterCloseSender closeAsync() noexcept;

        [[nodiscard]] LogRouterStatistics statistics() const noexcept;

    private:
        friend void detail::subscribeLogRouterClose(
            LogRouter&,
            lux::cxx::move_only_function<void(LogRouterStatistics)>)
            noexcept;
        struct State;
        std::shared_ptr<State> state_;
    };
}
