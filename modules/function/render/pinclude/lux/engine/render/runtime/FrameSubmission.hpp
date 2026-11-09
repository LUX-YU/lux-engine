#pragma once

#include <lux/engine/render/commands/Frame.hpp>

namespace lux::render::runtime_detail
{
    // Fixed-address input/settlement record. stdexec owns every connected operation;
    // the sole terminal borrow belongs to the accepted SubmitFrame operation.
    struct FrameRecord final
    {
        enum class EStage : std::uint8_t
        {
            EMPTY,
            QUEUED,
            APPLYING,
            PREPARING_PROGRAMS,
            ACQUIRING_OUTPUTS,
            SUBMITTING
        };

        EStage stage = EStage::EMPTY;
        RenderFrame input;
        std::size_t phase = 0;
        std::size_t dispatched = 0;
        std::size_t pending = 0;
        std::uint32_t slot = UINT32_MAX;
        std::uint32_t failed_entry = UINT32_MAX;
        RenderError error{};
        bool outcome_unknown = false;
        void* receiver = nullptr;
        void (*complete)(void*, FrameCompletion) noexcept = nullptr;
    };
} // namespace lux::render::runtime_detail
