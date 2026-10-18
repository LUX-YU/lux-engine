#pragma once

#include "ResponseOutbox.hpp"
#include <atomic>
#include <lux/engine/render/resource/AsyncWork.hpp>
#include <lux/engine/render/runtime/CommandBinding.hpp>
#include <mutex>

#ifdef LUX_RUNTIME_ASYNC_TEST_SEAM
namespace lux::render::runtime_detail::test
{
    void completionPublished() noexcept;
}
#endif

namespace lux::render::runtime_detail
{
    struct CommandCompletion;

    // Existing completion channel: result records are acquired before work starts.
    // Only ready nodes are linked. This short CPU notification lock never enters IPC SPSC publication.
    class CompletionQueue final
    {
    public:
        void push(CommandCompletion&) noexcept;
        [[nodiscard]] CommandCompletion* tryPop() noexcept;

        [[nodiscard]] std::size_t size() const noexcept
        {
            std::lock_guard lock(mutex_);
            return size_;
        }

    private:
        mutable std::mutex mutex_;
        CommandCompletion* head_ = nullptr;
        CommandCompletion* tail_ = nullptr;
        std::size_t size_ = 0;
    };

#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
    namespace test
    {
        void beforeDispatch() noexcept;
        void* completionRegistered(const CommandCompletion*, RequestId) noexcept;
        void* completionPostBegin(void*) noexcept;
        void completionPostEnd(void*, bool accepted) noexcept;
        void* completionDrainBegin(const CompletionQueue&) noexcept;
        void* completionPopped(void*) noexcept;
        void completionAdoptBegin(void*) noexcept;
        void completionAdoptEnd(void*) noexcept;
        void completionDrainEnd(void*, std::size_t consumed) noexcept;
    } // namespace test
#endif

    // Owns only routing and completed output. Native continuations borrow this landing place;
    // connected OperationState ownership belongs exclusively to stdexec::spawn.
    struct CommandCompletion final
    {
        ResponseRoute response_route_;
        platform::ipc::IpcMessage result_message_;
        CompletionQueue* events = nullptr;
        std::atomic<std::size_t>* publishers = nullptr;
        CommandCompletion* ready_next = nullptr;
        void* continuation = nullptr;
        CpuResume resume = nullptr;
        RenderResult<void> continuation_result;
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
        void* measurement = nullptr;
#endif

        [[nodiscard]] CompletionPost post() noexcept
        {
            return {
                this,
                [](void* owner, void* next, CpuResume callback, RenderResult<void> result) noexcept
                {
                    auto& slot = *static_cast<CommandCompletion*>(owner);
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
                    auto* measurement = slot.measurement;
#endif
                    auto* events = slot.events;
                    auto* publishers = slot.publishers;
                    publishers->fetch_add(1, std::memory_order_acq_rel);
                    slot.continuation = next;
                    slot.resume = callback;
                    slot.continuation_result = std::move(result);
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
                    auto* cost_record = test::completionPostBegin(measurement);
                    events->push(slot);
                    test::completionPostEnd(cost_record, true);
#else
                    events->push(slot); // The result record is its own already-acquired notification landing place.
#endif
                    // No access to slot after publication: the consumer may immediately destroy it.
#ifdef LUX_RUNTIME_ASYNC_TEST_SEAM
                    test::completionPublished();
#endif
                    // The queue notifies after releasing its mutex. Keep Impl/queue alive until that tail returns.
                    publishers->fetch_sub(1, std::memory_order_acq_rel);
                }
            };
        }
    };

    inline void CompletionQueue::push(CommandCompletion& slot) noexcept
    {
        {
            std::lock_guard lock(mutex_);
            slot.ready_next = nullptr;
            if (tail_)
            {
                tail_->ready_next = &slot;
            }
            else
            {
                head_ = &slot;
            }
            tail_ = &slot;
            ++size_;
        }
        platform::ipc::IpcCore::instance().notifyWork();
    }

    inline CommandCompletion* CompletionQueue::tryPop() noexcept
    {
        std::lock_guard lock(mutex_);
        auto* result = head_;
        if (!result)
        {
            return nullptr;
        }
        head_ = result->ready_next;
        if (!head_)
        {
            tail_ = nullptr;
        }
        result->ready_next = nullptr;
        --size_;
        return result;
    }

} // namespace lux::render::runtime_detail

#ifdef LUX_RUNTIME_TEST_SEAM
namespace lux::render::runtime_detail::test
{
    bool observeGpuCompletion() noexcept;
}
#endif
