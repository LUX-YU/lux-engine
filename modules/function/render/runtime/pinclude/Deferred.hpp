#pragma once

#include "ResponseOutbox.hpp"
#include <atomic>
#include <lux/engine/render/resource/AsyncWork.hpp>
#include <lux/engine/render/runtime/CommandBinding.hpp>
#include <lux/engine/core/execution/RunLoop.hpp>

#ifdef LUX_RUNTIME_ASYNC_TEST_SEAM
namespace lux::render::runtime_detail::test
{
    void completionPublished() noexcept;
}
#endif

namespace lux::render::runtime_detail
{
    struct CommandCompletion;

#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
    namespace test
    {
        void beforeDispatch() noexcept;
        void* completionRegistered(const CommandCompletion*, RequestId) noexcept;
        void* completionPostBegin(void*) noexcept;
        void completionPostEnd(void*, bool accepted) noexcept;
        void* completionDrainBegin(const execution::RunLoop&) noexcept;
        void* completionPopped(void*) noexcept;
        void completionAdoptBegin(void*) noexcept;
        void completionAdoptEnd(void*) noexcept;
        void completionDrainEnd(void*, std::size_t consumed) noexcept;
    } // namespace test
#endif

    // Owns only routing and completed output. Native continuations borrow this landing place;
    // connected OperationState ownership belongs exclusively to stdexec::spawn.
    struct CommandCompletion final : execution::RunLoop::Work
    {
        CommandCompletion() noexcept : Work(deliver) {}
        static void deliver(execution::RunLoop::Work&) noexcept;

        ResponseRoute response_route_;
        platform::ipc::IpcMessage result_message_;
        execution::RunLoop* loop = nullptr;
        ResponseOutbox* outbox = nullptr;
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
                    auto* loop = slot.loop;
                    slot.continuation = next;
                    slot.resume = callback;
                    slot.continuation_result = std::move(result);
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
                    auto* cost_record = test::completionPostBegin(measurement);
                    loop->post(slot);
                    test::completionPostEnd(cost_record, true);
#else
                    loop->post(slot); // The result record is its own already-acquired notification landing place.
#endif
                    // No access to slot after publication. RunLoop protects the wakeup tail.
                }
            };
        }
    };

} // namespace lux::render::runtime_detail

#ifdef LUX_RUNTIME_TEST_SEAM
namespace lux::render::runtime_detail::test
{
    bool observeGpuCompletion() noexcept;
}
#endif
