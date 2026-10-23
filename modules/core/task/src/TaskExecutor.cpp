#include <lux/engine/task/TaskExecutor.hpp>

#include <algorithm>
#include <new>

namespace lux::task
{
    struct TaskExecutor::Impl final
    {
        Impl(execution::CpuScheduler value, std::size_t capacity) noexcept : cpu(value), minimum_task_capacity(capacity)
        {
        }

        ~Impl() noexcept
        {
            if (executing.load(std::memory_order_acquire))
            {
                std::terminate();
            }
            ::operator delete(workspace, std::align_val_t(alignment));
        }

        execution::CpuScheduler cpu;
        std::atomic_bool executing{};
        void* workspace{};
        std::size_t minimum_task_capacity{};
        std::size_t bytes{};
        std::size_t alignment{alignof(std::max_align_t)};
    };

    TaskExecutor::TaskExecutor(execution::CpuScheduler cpu, std::size_t initial_task_capacity) noexcept
        : impl_(std::make_unique<Impl>(cpu, initial_task_capacity))
    {
    }

    TaskExecutor::~TaskExecutor() noexcept = default;
    TaskExecutor::TaskExecutor(TaskExecutor&&) noexcept = default;
    TaskExecutor& TaskExecutor::operator=(TaskExecutor&&) noexcept = default;

    std::size_t TaskExecutor::minimumTaskCapacity() const noexcept
    {
        return impl_->minimum_task_capacity;
    }

    TaskExecutor::Result TaskExecutor::setMinimumTaskCapacity(std::size_t minimum_task_capacity) noexcept
    {
        if (!acquire())
        {
            return lux::cxx::unexpected(TaskExecutorFailure{});
        }
        impl_->minimum_task_capacity = std::max(minimum_task_capacity, impl_->minimum_task_capacity);
        release();
        return {};
    }

    bool TaskExecutor::acquire() noexcept
    {
        return !impl_->executing.exchange(true, std::memory_order_acq_rel);
    }

    void TaskExecutor::release() noexcept
    {
        impl_->executing.store(false, std::memory_order_release);
    }

    execution::CpuScheduler TaskExecutor::cpu() const noexcept
    {
        return impl_->cpu;
    }

    void* TaskExecutor::workspace(std::size_t count, std::size_t stride, std::size_t alignment) noexcept
    {
        impl_->minimum_task_capacity = std::max(impl_->minimum_task_capacity, count);
        const auto bytes = impl_->minimum_task_capacity * stride;
        if (bytes > impl_->bytes || alignment > impl_->alignment)
        {
            auto* value = ::operator new(bytes, std::align_val_t(alignment));
            ::operator delete(impl_->workspace, std::align_val_t(impl_->alignment));
            impl_->workspace = value;
            impl_->bytes = bytes;
            impl_->alignment = alignment;
        }
        return impl_->workspace;
    }

    TaskGraphSender TaskExecutor::schedule(const TaskGraph& graph) noexcept
    {
        return {*this, graph};
    }

    TaskExecutor::Result TaskExecutor::execute(const TaskGraph& graph) noexcept
    {
        auto completed = stdexec::sync_wait(
            schedule(graph) | stdexec::then([]() noexcept { return Result{}; }) |
            stdexec::upon_error(
                [](TaskExecutorFailure failure) noexcept -> Result { return lux::cxx::unexpected(failure); }
            )
        );
        return std::move(std::get<0>(*completed));
    }
} // namespace lux::task
