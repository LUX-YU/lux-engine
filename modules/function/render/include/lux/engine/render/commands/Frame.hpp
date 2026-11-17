#pragma once

#include <lux/engine/render/frame/RenderFrame.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/frame/FrameLayout.hpp>
#include <span>

namespace lux::render
{
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
        EFrameOutcome outcome = EFrameOutcome::REJECTED;
        // Accepted FrameExecution batches, including replica preparation. Independent asset
        // uploads retain their own Command outcomes and are not counted as frame submissions.
        std::uint32_t submitted_batches = 0;
        bool requires_resynchronization = false;
        std::uint32_t failed_entry = UINT32_MAX;
    };

    // One sealed frame, never an unbounded Begin/End message stream. Completion returns the CPU
    // input borrow and submission outcome; it does not certify GPU completion or presentation.
    struct SubmitFrame final
    {
        static constexpr std::string_view name = "render.frame.submit";
        static constexpr bool requires_frame_boundary = false;
        using Request = RenderFrame;
        using Response = FrameCompletion;
        static FrameData* frameData(const Request& request) noexcept { return request.data; }
    };

    struct QueryFrameInputLayout final
    {
        static constexpr std::string_view name = "render.frame.query_input_layout";
        static constexpr bool requires_frame_boundary = false;
        using Request = AttachmentId;
        struct Response final
        {
            RenderError error{};
            FrameLayout layout;
        };
    };
} // namespace lux::render
