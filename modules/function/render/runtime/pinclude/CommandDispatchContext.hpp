#pragma once

#include "Deferred.hpp"

namespace lux::render::runtime_detail
{
    struct CommandDispatchContext final
    {
        platform::ipc::IpcMessage& request_message_;
        ResponseRoute response_route_;
        ResponseOutbox& response_outbox_;
        stdexec::counting_scope& scope_;
        CompletionQueue& completion_queue_;
        std::atomic<std::size_t>& publishers_;
    };
} // namespace lux::render::runtime_detail
