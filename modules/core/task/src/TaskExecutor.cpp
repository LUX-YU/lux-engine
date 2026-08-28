#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/task/TaskExecutorDetail.hpp>
#include <lux/engine/task/TaskExecutorFailureInjection.hpp>

#include <new>
#include <utility>

namespace lux::task
{
    struct TaskExecutor::Impl final : detail::TaskExecutorImpl
    {
        explicit Impl(TaskExecutorConfig config) : detail::TaskExecutorImpl(config)
        {
        }
    };

    lux::cxx::expected<TaskExecutor, TaskExecutorFailure> TaskExecutor::create(TaskExecutorConfig config) noexcept
    {
        if (detail::consumeTaskExecutorFailureForTest(detail::ETaskExecutorFailurePoint::ALLOCATION))
            return lux::cxx::unexpected(TaskExecutorFailure{ETaskExecutorError::ALLOCATION_FAILURE});

        try
        {
            auto impl = std::make_unique<Impl>(config);
            auto started = impl->startWorkers();
            if (!started)
            {
                return lux::cxx::unexpected(started.error());
            }
            return TaskExecutor(std::move(impl));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(TaskExecutorFailure{ETaskExecutorError::ALLOCATION_FAILURE});
        }
        catch (...)
        {
            return lux::cxx::unexpected(TaskExecutorFailure{ETaskExecutorError::WORKER_CREATION_FAILURE});
        }
    }

    TaskExecutor::TaskExecutor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

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

    lux::cxx::expected<void, TaskExecutorFailure> TaskExecutor::reserve(std::size_t task_capacity) noexcept
    {
        return impl_->reserve(task_capacity);
    }

    lux::cxx::expected<void, TaskExecutorFailure> TaskExecutor::execute(const TaskGraph& graph) noexcept
    {
        return impl_->execute(graph);
    }
}
