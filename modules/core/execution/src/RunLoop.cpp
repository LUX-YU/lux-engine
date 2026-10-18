#include <lux/engine/core/execution/RunLoop.hpp>
#include <exception>

namespace lux::execution
{
    RunLoop::RunLoop(void (*wakeup)(void*) noexcept, void* context) noexcept
        : wakeup_(wakeup ? wakeup : [](void*) noexcept {}), wakeup_context_(context)
    {}

    RunLoop::~RunLoop()
    {
        if (!empty() || activePublishers() != 0) std::terminate();
    }

    void RunLoop::post(Work& work) noexcept
    {
        active_publishers_.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard lock(mutex_);
            work.next_ = nullptr;
            if (tail_) tail_->next_ = &work;
            else head_ = &work;
            tail_ = &work;
            ++size_;
        }
        // The consumer may already have destroyed work. Only the host-owned loop is accessed here.
        wakeup_(wakeup_context_);
        active_publishers_.fetch_sub(1, std::memory_order_acq_rel);
    }

    std::size_t RunLoop::executeReady(std::size_t budget) noexcept
    {
        std::size_t consumed = 0;
        while (consumed < budget)
        {
            Work* work;
            {
                std::lock_guard lock(mutex_);
                work = head_;
                if (!work) break;
                head_ = work->next_;
                if (!head_) tail_ = nullptr;
                work->next_ = nullptr;
                --size_;
            }
            ++consumed;
            work->execute_(*work);
            // No node access after execution: callbacks may repost or destroy their own node.
        }
        return consumed;
    }

    std::size_t RunLoop::size() const noexcept
    {
        std::lock_guard lock(mutex_);
        return size_;
    }
} // namespace lux::execution
