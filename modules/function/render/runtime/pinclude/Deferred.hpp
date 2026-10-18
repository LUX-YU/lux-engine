#pragma once

#include "ResponseOutbox.hpp"
#include <atomic>
#include <lux/engine/core/execution/RunLoop.hpp>
#include <lux/engine/render/runtime/CommandBinding.hpp>

namespace lux::render::runtime_detail
{
    struct CommandCompletion;

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
    };

} // namespace lux::render::runtime_detail
