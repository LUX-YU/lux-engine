#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <stop_token>
#include <stdexec/execution.hpp>

namespace lux::execution::detail
{
    struct TaskRecord;
    struct TaskCapacityWaiter;

    using TaskGroupAssociation =
        decltype(std::declval<stdexec::simple_counting_scope&>().get_token().try_associate());

    struct TaskGroupJoinReceiver final
    {
        using receiver_concept = stdexec::receiver_t;
        std::atomic_bool& joined;

        [[nodiscard]] auto get_env() const noexcept
        {
            return stdexec::env{stdexec::prop{stdexec::get_scheduler, stdexec::inline_scheduler{}}};
        }

        void set_value() && noexcept
        {
            joined.store(true, std::memory_order_release);
        }
    };

    // Cancellation and idleness belong to the business group. stdexec owns execution states
    // and join delivery; active_operations is only the open group's non-closing idle query.
    struct TaskGroup final
    {
        using JoinOperation = stdexec::connect_result_t<
            decltype(std::declval<stdexec::simple_counting_scope&>().join()),
            TaskGroupJoinReceiver
        >;

        std::atomic_bool accepting{true};
        std::atomic_size_t active_operations{};
        std::stop_source stop;
        // Group membership is protected by TaskRuntime::mutex, not the execution scope.
        TaskRecord* first_task{};
        std::size_t task_count{};
        TaskCapacityWaiter* capacity_waiters{};
        bool stop_requested{};
        stdexec::simple_counting_scope scope;
        std::atomic_bool joined{};
        std::atomic_bool joining{};
        JoinOperation join_operation{stdexec::connect(scope.join(), TaskGroupJoinReceiver{joined})};

        void close() noexcept
        {
            accepting.store(false, std::memory_order_release);
            scope.close();
            if (!joining.exchange(true, std::memory_order_acq_rel))
            {
                stdexec::start(join_operation);
            }
        }
    };
} // namespace lux::execution::detail
