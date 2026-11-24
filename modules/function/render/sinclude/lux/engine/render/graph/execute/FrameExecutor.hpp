#pragma once

#include <NRI.h>
#include <lux/engine/render/core/Error.hpp>
#include <cstdint>
#include <span>
#include <lux/engine/render/visibility.h>

namespace lux::render::detail
{
    class DeviceSession;
    struct FrozenRecordingFrame;
    struct BatchRecording;

    // A worker records one native batch from a frozen frame. Each command buffer has independent binding state.
    class LUX_RENDER_PUBLIC FrameExecutor final
    {
    public:
        [[nodiscard]] static RenderResult<std::span<nri::CommandBuffer* const>>
        recordBatch(const FrozenRecordingFrame&, const DeviceSession&, std::uint32_t batch) noexcept;

    private:
        static void recordBatchPart(
            const FrozenRecordingFrame&, const nri::CoreInterface&, nri::CommandBuffer&, const BatchRecording&
        ) noexcept;

        static void
        recordHistoryInitialization(
            const FrozenRecordingFrame&,
            const nri::CoreInterface&,
            nri::CommandBuffer&
        ) noexcept;
    };
} // namespace lux::render::detail
