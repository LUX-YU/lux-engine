#pragma once

#include "Request.hpp"
#include <lux/engine/render/protocol/AsyncCommand.hpp>
#include <lux/engine/render/resource/AsyncWork.hpp>
#include <lux/engine/render/native/vulkan/CommandCompletion.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/cxx/concurrent/BlockingQueue.hpp>
#include <atomic>

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

    struct CompletionEvent final
    {
        SessionIdentity session;
        CompletionId id;
    };

    using CompletionQueue = cxx::BlockingRingQueue<CompletionEvent>;

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
    }
#endif

    struct DeferredCompletion;
    using DeferredSlots = cxx::StableSlotMap<DeferredCompletion, CompletionTag>;

    struct GpuCompletion final
    {
        vulkan::SubmissionQueue* queue = nullptr;
        std::uint64_t submitted_before_reserve = 0;
        vulkan::SubmissionCallback callback;
        vulkan::GpuWork work;
        platform::ipc::IpcMessage reply;
        std::size_t reply_charge = 0;
        RenderRequest* reserved_request = nullptr;
    };

    struct ReadbackCompletion final
    {
        // Null after topology cancellation: the old Context may retire before this callback dispatches.
        vulkan::ExecutionContext* context = nullptr;
        vulkan::SubmissionCallback callback;
    };

    struct CpuCompletion final
    {
        void (*destroy)(void*) noexcept = nullptr;
        void* operation = nullptr;
        platform::ipc::IpcMessage reply;
        std::size_t reply_charge = 0;
        ECommandOutcome outcome = ECommandOutcome::OUTCOME_UNKNOWN;
        RenderError error{};
        bool compiling = false;
        RenderResult<void> (*adopt)(void*, RenderRequest&) noexcept = nullptr;

        CpuCompletion() noexcept = default;
        CpuCompletion(const CpuCompletion&) = delete;
        CpuCompletion& operator=(const CpuCompletion&) = delete;
        ~CpuCompletion() noexcept { if (operation) destroy(operation); }
    };

    using VCompletionCondition = std::variant<std::monostate, ReadbackCompletion, GpuCompletion, CpuCompletion>;

    // The only pending request owner. Its fixed-address slot reserves one CPU event landing place.
    struct DeferredCompletion final
    {
        std::optional<RenderRequest> request;
        stdexec::inplace_stop_source stop;
        // Sender operations may retain a stop_callback until destruction; its source outlives them.
        VCompletionCondition condition;
        CompletionQueue* events = nullptr;
        std::atomic<std::size_t>* publishers = nullptr;
        void* server = nullptr;
        SessionIdentity session{};
        CompletionId id;
        void* continuation = nullptr;
        CpuResume resume = nullptr;
        RenderResult<void> cpu_result;

        DeferredCompletion() noexcept = default;
        [[nodiscard]] CompletionPost post() noexcept
        {
            return {this, [](void* owner, void* next, CpuResume callback, RenderResult<void> result) noexcept {
                auto& slot = *static_cast<DeferredCompletion*>(owner);
                const CompletionEvent event{slot.session, slot.id};
                auto* events = slot.events;
                auto* publishers = slot.publishers;
                publishers->fetch_add(1, std::memory_order_acq_rel);
                slot.continuation = next;
                slot.resume = callback;
                slot.cpu_result = std::move(result);
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
                auto* cost_record = test::completionPostBegin(event.id);
                const auto published = events->tryPush(event);
                test::completionPostEnd(cost_record, published == cxx::EQueuePushResult::ACCEPTED);
                if (published != cxx::EQueuePushResult::ACCEPTED) std::terminate();
#else
                // One event per live slot; admission reserved matching queue capacity.
                if (events->tryPush(event) != cxx::EQueuePushResult::ACCEPTED) std::terminate();
#endif
                // No access to slot after publication: the consumer may immediately destroy it.
#ifdef LUX_RUNTIME_ASYNC_TEST_SEAM
                test::completionPublished();
#endif
                // The queue notifies after releasing its mutex. Keep Impl/queue alive until that tail returns.
                publishers->fetch_sub(1, std::memory_order_acq_rel);
            }};
        }
    };

}

#ifdef LUX_RUNTIME_TEST_SEAM
namespace lux::render::runtime_detail::test
{
    bool observeGpuCompletion() noexcept;
    void gpuReplyPublicationStarted() noexcept;
}
#endif
