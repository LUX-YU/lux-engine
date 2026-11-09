#include <lux/engine/task/TaskExecutor.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <new>

namespace lux::task
{
    struct TaskExecutor::Impl final
    {
        explicit Impl(execution::CpuScheduler scheduler) noexcept : cpu_scheduler(scheduler)
        {
        }

        ~Impl() noexcept
        {
            // Destruction while a connected graph still borrows the workspace is a contract violation.
            if (executing.load(std::memory_order_acquire))
            {
                std::terminate();
            }
            ::operator delete(workspace, std::align_val_t(workspace_alignment));
        }

        execution::CpuScheduler cpu_scheduler;
        std::atomic_bool        executing{};
        void*                   workspace{};
        std::size_t             workspace_bytes{};
        std::size_t             workspace_alignment{alignof(std::max_align_t)};
    };

    TaskExecutor::TaskExecutor(execution::CpuScheduler scheduler) noexcept : impl_(std::make_unique<Impl>(scheduler))
    {
    }

    TaskExecutor::~TaskExecutor() noexcept = default;

    bool TaskExecutor::acquire() noexcept
    {
        return !impl_->executing.exchange(true, std::memory_order_acq_rel);
    }

    void TaskExecutor::release() noexcept
    {
        impl_->executing.store(false, std::memory_order_release);
    }

    execution::CpuScheduler TaskExecutor::cpuScheduler() const noexcept
    {
        return impl_->cpu_scheduler;
    }

    void* TaskExecutor::workspace(std::size_t count, std::size_t stride, std::size_t alignment) noexcept
    {
        const auto bytes = count * stride;
        if (bytes > impl_->workspace_bytes || alignment > impl_->workspace_alignment)
        {
            const auto allocation_bytes = std::max(bytes, impl_->workspace_bytes);
            const auto allocation_alignment = std::max(alignment, impl_->workspace_alignment);
            auto* memory = ::operator new(allocation_bytes, std::align_val_t(allocation_alignment));
            ::operator delete(impl_->workspace, std::align_val_t(impl_->workspace_alignment));
            impl_->workspace = memory;
            impl_->workspace_bytes = allocation_bytes;
            impl_->workspace_alignment = allocation_alignment;
        }
        return impl_->workspace;
    }

    TaskGraphSender TaskExecutor::schedule(const TaskGraph& graph) noexcept
    {
        return {*this, graph};
    }
} // namespace lux::task
