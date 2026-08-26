#pragma once

#include <lux/engine/task/Task.hpp>
#include <lux/engine/task/TaskCallable.hpp>
#include <lux/engine/task/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace lux::task
{
    class TaskGraphBuilder;

    namespace detail
    {
        struct TaskExecutorImpl;
    }

    /**
     * Immutable compiled task DAG.
     *
     * Build-time resource declarations and explicit dependency properties are not
     * retained. Runtime retains only what completion-driven execution needs:
     * callable records, initial prerequisite counts, successor CSR and roots.
     */
    class LUX_CORE_TASK_PUBLIC TaskGraph final
    {
    public:
        TaskGraph() = default;
        ~TaskGraph() = default;

        TaskGraph(TaskGraph&&) noexcept = default;
        TaskGraph& operator=(TaskGraph&&) noexcept = default;

        TaskGraph(const TaskGraph&) = delete;
        TaskGraph& operator=(const TaskGraph&) = delete;

        [[nodiscard]] std::size_t taskCount() const noexcept
        {
            return tasks_.size();
        }

        [[nodiscard]] std::size_t dependencyCount() const noexcept
        {
            return successors_.size();
        }

        [[nodiscard]] std::size_t rootTaskCount() const noexcept
        {
            return roots_.size();
        }

        [[nodiscard]] std::size_t lifetimePinCount() const noexcept
        {
            return lifetime_pins_.size();
        }

    private:
        struct TaskRecord final
        {
            TaskCallable callable;
            ETaskAffinity affinity{ETaskAffinity::WORKER};
        };

        // Destruction is reverse declaration order: task callables die before pins,
        // so dynamic code remains resident while callable destructors execute.
        std::vector<std::shared_ptr<const void>> lifetime_pins_;
        std::vector<TaskRecord> tasks_;
        std::vector<std::uint32_t> initial_dependencies_;
        std::vector<std::uint32_t> successor_offsets_;
        std::vector<std::uint32_t> successors_;
        std::vector<std::uint32_t> roots_;

        friend class TaskGraphBuilder;
        friend struct detail::TaskExecutorImpl;
    };
}
