#pragma once

#include "ResponseOutbox.hpp"
#include <atomic>
#include <lux/cxx/container/StableSlotMap.hpp>
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
    struct CompletionTag;
    using CompletionId = cxx::SlotKey<CompletionTag>;

    struct PendingOperation;

    // Existing completion channel: nodes are reserved in stable pending slots before work starts.
    // Only ready nodes are linked. This short CPU notification lock never enters IPC SPSC publication.
    class CompletionQueue final
    {
    public:
        void push(PendingOperation&) noexcept;
        [[nodiscard]] PendingOperation* tryPop() noexcept;

        [[nodiscard]] std::size_t size() const noexcept
        {
            std::lock_guard lock(mutex_);
            return size_;
        }

    private:
        mutable std::mutex mutex_;
        PendingOperation* head_ = nullptr;
        PendingOperation* tail_ = nullptr;
        std::size_t size_ = 0;
    };

#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
    namespace test
    {
        void beforeDispatch() noexcept;
        void completionRegistered(CompletionId, RequestId) noexcept;
        void* completionPostBegin(CompletionId) noexcept;
        void completionPostEnd(void*, bool accepted) noexcept;
        void* completionDrainBegin(const CompletionQueue&) noexcept;
        void* completionPopped(CompletionId) noexcept;
        void completionAdoptBegin(void*) noexcept;
        void completionAdoptEnd(void*) noexcept;
        void completionDrainEnd(void*, std::size_t consumed) noexcept;
    } // namespace test
#endif

    struct PendingOperation;
    using PendingSlots = cxx::StableSlotMap<PendingOperation, CompletionTag>;

    // The only pending operation owner. Its fixed-address slot reserves one completion event landing place.
    struct PendingOperation final
    {
        ResponseRoute response_route_;
        stdexec::inplace_stop_source stop;
        // Sender operations may retain a stop_callback until destruction; its source outlives them.
        void* operation_state_ = nullptr;
        void (*destroy_operation_)(void*) noexcept = nullptr;
        platform::ipc::IpcMessage result_message_;
        CompletionQueue* events = nullptr;
        std::atomic<std::size_t>* publishers = nullptr;
        CompletionId id;
        PendingOperation* ready_next = nullptr;
        void* continuation = nullptr;
        CpuResume resume = nullptr;
        RenderResult<void> continuation_result;

        PendingOperation() noexcept = default;

        ~PendingOperation() noexcept
        {
            if (operation_state_)
            {
                destroy_operation_(operation_state_);
            }
        }

        [[nodiscard]] CompletionPost post() noexcept
        {
            return {
                this,
                [](void* owner, void* next, CpuResume callback, RenderResult<void> result) noexcept
                {
                    auto& slot = *static_cast<PendingOperation*>(owner);
                    const auto id = slot.id;
                    auto* events = slot.events;
                    auto* publishers = slot.publishers;
                    publishers->fetch_add(1, std::memory_order_acq_rel);
                    slot.continuation = next;
                    slot.resume = callback;
                    slot.continuation_result = std::move(result);
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
                    auto* cost_record = test::completionPostBegin(id);
                    events->push(slot);
                    test::completionPostEnd(cost_record, true);
#else
                    events->push(slot); // The stable slot is its own already-acquired notification landing place.
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

    inline void CompletionQueue::push(PendingOperation& slot) noexcept
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

    inline PendingOperation* CompletionQueue::tryPop() noexcept
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
