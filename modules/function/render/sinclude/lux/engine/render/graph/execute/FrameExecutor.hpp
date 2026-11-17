#pragma once

#include <NRI.h>
#include <lux/engine/render/core/Error.hpp>
#include <cstdint>
#include <span>
#include <lux/engine/render/visibility.h>

namespace lux::render::detail
{
    class RenderGraphInstance;
    struct BindingState;
    struct FrameRecordInput;

    // Encoding algorithms only. The instance owns exclusive allocators and Begin/End, outside Record.
    class LUX_RENDER_PUBLIC FrameExecutor final
    {
        friend class RenderGraphInstance;

        [[nodiscard]] static RenderResult<void>
        recordBatchPart(
            const FrameRecordInput&,
            const nri::CoreInterface&,
            nri::CommandBuffer&,
            std::uint32_t batch,
            bool output_tail,
            BindingState&
        ) noexcept;

        static void
        recordHistoryInitialization(
            const FrameRecordInput&,
            const nri::CoreInterface&,
            nri::CommandBuffer&
        ) noexcept;
    };
} // namespace lux::render::detail
