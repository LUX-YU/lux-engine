#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskExecutorDetail.hpp>

#include <utility>

namespace lux::task
{
    struct TaskExecutor::Impl final : detail::TaskExecutorImpl
    {
        explicit Impl(TaskExecutorConfig config)
            : detail::TaskExecutorImpl(config)
        {
        }
    };

    TaskExecutor::TaskExecutor(TaskExecutorConfig config)
        : impl_(std::make_unique<Impl>(config))
    {
    }

    TaskExecutor::~TaskExecutor() noexcept = default;
    TaskExecutor::TaskExecutor(TaskExecutor&&) noexcept = default;
    TaskExecutor& TaskExecutor::operator=(TaskExecutor&&) noexcept = default;

    std::uint32_t TaskExecutor::workerCount() const noexcept
    {
        return impl_ ? impl_->worker_count : 0U;
    }

    std::size_t TaskExecutor::taskCapacity() const noexcept
    {
        return impl_ ? impl_->task_capacity : 0U;
    }

    lux::cxx::expected<void, TaskExecutorFailure> TaskExecutor::reserve(
        std::size_t task_capacity
    ) noexcept
    {
        return impl_->reserve(task_capacity);
    }

    lux::cxx::expected<void, TaskExecutorFailure> TaskExecutor::execute(
        const TaskGraph& graph
    ) noexcept
    {
        return impl_->execute(graph);
    }
}
