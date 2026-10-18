#pragma once

#include "ResponseOutbox.hpp"
#include <atomic>
#include <lux/engine/core/execution/RunLoop.hpp>
#include <lux/engine/render/runtime/CommandBinding.hpp>

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

    // Owns only routing and completed output;
    // connected OperationState ownership belongs exclusively to stdexec::spawn.
    struct CommandCompletion final : execution::RunLoop::Work
    {
        CommandCompletion() noexcept : Work(deliver) {}

        static void deliver(execution::RunLoop::Work&) noexcept;

        ResponseRoute response_route_;
        platform::ipc::IpcMessage result_message_;
        execution::RunLoop* loop = nullptr;
        ResponseOutbox* outbox = nullptr;
#ifdef LUX_RUNTIME_ASYNC_COST_SEAM
        void* measurement = nullptr;
#endif
    };

} // namespace lux::render::runtime_detail

#ifdef LUX_RUNTIME_TEST_SEAM
namespace lux::render::runtime_detail::test
{
    bool observeGpuCompletion() noexcept;
}
#endif
