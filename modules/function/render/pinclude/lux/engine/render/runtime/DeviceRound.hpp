#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/runtime/FrameOperation.hpp>

namespace lux::render
{
    class FrameData;
}

namespace lux::render::runtime_detail
{
    struct DeviceRound final
    {
        enum class EStage : std::uint8_t { ACQUIRING_OUTPUTS, SUBMITTING };

        std::optional<detail::FrameOperation> operation;
        EStage stage = EStage::ACQUIRING_OUTPUTS;
        bool pending = false;
        RenderError error;

        [[nodiscard]] std::uint32_t slot() const noexcept { return operation->context().slot_index; }
    };

    struct JobExecution;

    // Borrowed from the connected UpdateFrameData operation until its terminal response.
    struct InputUpdate final
    {
        RenderJobId job;
        const FrameData* data;
        JobExecution* execution;
        void* receiver;
        void (*complete)(void*, AckResponse) noexcept;
    };
} // namespace lux::render::runtime_detail
