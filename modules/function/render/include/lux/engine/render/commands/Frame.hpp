#pragma once

#include <lux/engine/platform/ipc/IpcMessage.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <span>

namespace lux::render
{
    struct FrameCommand final
    {
        CommandId command;
        RequestId request_id;
        platform::ipc::IpcMessage request;
    };

    // Commands in one group must be independent. The next group starts after all of them settle.
    struct FrameCommandGroup final
    {
        std::span<FrameCommand> commands;
    };

    // The Client retains these arrays through FrameCompletion. Accepted dispatch may move Request
    // objects out of their messages; the Client must not access them during this exclusive borrow.
    // Disconnect/abandon is not completion. Without a Response, retain inputs until the application
    // has stopped/joined the Server and its accepted producers.
    struct FrameSubmission final
    {
        std::uint64_t sequence = 0;
        std::span<const FrameCommandGroup> groups;
        bool resynchronize = false;
    };

    enum class EFrameOutcome : std::uint8_t
    {
        REJECTED,
        NOT_SUBMITTED,
        NO_DRAW,
        SUBMITTED,
        PARTIALLY_SUBMITTED,
        UNKNOWN
    };

    struct FrameCompletion final
    {
        RenderError error{};
        std::uint64_t sequence = 0;
        EFrameOutcome outcome = EFrameOutcome::REJECTED;
        // Accepted FrameExecution batches, including replica preparation. Independent asset
        // uploads retain their own Command outcomes and are not counted as frame submissions.
        std::uint32_t submitted_batches = 0;
        bool requires_resynchronization = false;
    };

    // One sealed frame, never an unbounded Begin/End message stream. Completion returns the CPU
    // input borrow and submission outcome; it does not certify GPU completion or presentation.
    struct SubmitFrame final
    {
        static constexpr std::string_view name = "render.frame.submit";
        static constexpr bool frame_scoped = false;
        using Request = FrameSubmission;
        using Response = FrameCompletion;
    };
} // namespace lux::render
