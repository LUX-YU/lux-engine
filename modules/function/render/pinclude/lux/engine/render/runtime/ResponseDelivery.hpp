#pragma once

#include <atomic>
#include <lux/engine/core/execution/RunLoop.hpp>
#include <lux/engine/render/runtime/CommandBinding.hpp>
#include <lux/engine/render/runtime/ResponseOutbox.hpp>

namespace lux::render::runtime_detail
{
    struct ResponseDelivery;

    // Owns only routing and completed output;
    // connected OperationState ownership belongs exclusively to stdexec::spawn.
    struct ResponseDelivery final : execution::RunLoop::Work
    {
        ResponseDelivery() noexcept : Work(deliver) {}

        static void deliver(execution::RunLoop::Work&) noexcept;

        ResponseRoute response_route_;
        platform::ipc::IpcMessage result_message_;
        execution::RunLoop* loop = nullptr;
        ResponseOutbox* outbox = nullptr;
    };

} // namespace lux::render::runtime_detail
