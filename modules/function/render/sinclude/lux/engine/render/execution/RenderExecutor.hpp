#pragma once

#include <NRI.h>
#include <cstdint>
#include <span>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render::detail
{
    class RenderGraphInstance;
    struct BindingState;
    class DeviceSession;
    struct RasterDrawList;

    class LUX_RENDER_PUBLIC RenderExecutor final
    {
    public:
        [[nodiscard]] static RenderResult<std::span<nri::CommandBuffer* const>>
        prepareBatch(RenderGraphInstance&, std::uint32_t frame_slot, std::uint32_t batch) noexcept;

    private:
        static void recordBatchPart(
            RenderGraphInstance&, nri::CommandBuffer&, std::uint32_t frame_slot, std::uint32_t batch,
            bool output_tail, BindingState&
        ) noexcept;

        static void
        recordRasterDraws(
            DeviceSession&,
            nri::CommandBuffer&,
            const RasterDrawList&,
            BindingState&
        ) noexcept;

        static void recordHistoryInitialization(RenderGraphInstance&, nri::CommandBuffer&) noexcept;
    };
} // namespace lux::render::detail
