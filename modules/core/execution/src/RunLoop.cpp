#include <algorithm>
#include <array>
#include <exception>
#include <lux/engine/core/execution/RunLoop.hpp>

namespace lux::execution
{
    RunLoop::RunLoop(void (*wakeup)(void*) noexcept, void* context) noexcept
        : wakeup_(wakeup ? wakeup : [](void*) noexcept {}), wakeup_context_(context)
    {
    }

    RunLoop::~RunLoop()
    {
        if (!empty() || activePublishers() != 0)
        {
            std::terminate();
        }
    }

    void RunLoop::post(Work& work) noexcept
    {
        publication_state_.fetch_add(1, std::memory_order_acq_rel);
        pending_.fetch_add(1, std::memory_order_release);
        if (!ready_.enqueue(&work))
        {
            // An accepted operation must deliver its terminal result. Ordinary heap OOM is fatal.
            std::terminate();
        }
        // The consumer may already have destroyed work. Only the loop is accessed after enqueue.
        wakeup_(wakeup_context_);
        constexpr auto returned = (std::uint64_t{1} << 32) - 1;
        publication_state_.fetch_add(returned, std::memory_order_acq_rel);
    }

    std::size_t RunLoop::executeReady(std::size_t limit) noexcept
    {
        std::array<Work*, 32> batch;
        std::size_t consumed = 0;
        while (consumed < limit)
        {
            const auto count =
                ready_.try_dequeue_bulk(consumer_, batch.begin(), std::min(batch.size(), limit - consumed));
            if (count == 0)
            {
                break;
            }
            for (std::size_t i = 0; i < count; ++i)
            {
                auto* work = batch[i];
                // The operation may release its own last pin during terminal destruction. Keep
                // its callback module loaded until control has returned to this Core function.
                auto code_lifetime = std::move(work->code_lifetime);
                work->execute(*work);
                // The callback may destroy its operation. No node access after execution.
            }
            pending_.fetch_sub(count, std::memory_order_acq_rel);
            consumed += count;
        }
        return consumed;
    }

    std::size_t RunLoop::size() const noexcept
    {
        return pending_.load(std::memory_order_acquire);
    }
} // namespace lux::execution
