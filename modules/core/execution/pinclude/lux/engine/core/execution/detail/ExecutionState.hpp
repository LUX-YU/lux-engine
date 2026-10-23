#pragma once

#include <condition_variable>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <mutex>
#include <semaphore>

namespace lux::execution::detail
{
    enum class EExecutionState : std::uint8_t
    {
        ACTIVE,
        STOPPING,
        JOINED
    };

    struct RequestQueue final
    {
        explicit RequestQueue(std::size_t capacity) : values(capacity) {}

        [[nodiscard]] bool push(ScheduleRequest* request) noexcept;
        [[nodiscard]] ScheduleRequest* pop() noexcept;

        std::vector<ScheduleRequest*> values;
        std::size_t head{}, tail{}, count{};
    };

    struct ExecutionState final
    {
        ExecutionState(ExecutionRuntimeConfig config, std::thread::id thread);
        void wake() noexcept;

        std::atomic<EExecutionState> phase{EExecutionState::ACTIVE};
        // Low bit closes unmonitored admission; each submission and spawn allocation owns one pair.
        std::atomic<std::uint64_t> execution_admission{};
        stdexec::simple_counting_scope execution_scope;
        std::unique_ptr<exec::static_thread_pool> cpu_pool;
        std::atomic<std::uint64_t> wake_epoch{};
        std::atomic_bool wake_pending{};
        std::binary_semaphore wake_signal{0};
        std::atomic<void (*)() noexcept> wake_callback{};
        RunLoop main_loop;
        bool dispatching_main{};
        bool stop_requested{};
        std::mutex blocking_mutex;
        std::condition_variable blocking_ready;
        RequestQueue blocking_queue;
        std::vector<std::jthread> blocking_workers;
        bool blocking_enabled{};
        std::thread::id main_thread;
    };
} // namespace lux::execution::detail
