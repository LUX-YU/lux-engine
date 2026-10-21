#pragma once

#include <NRI.h>
#include <cstdint>
#include <lux/engine/render/visibility.h>

namespace lux::render::detail
{
    class RenderGraphInstance;

    class LUX_RENDER_PUBLIC RenderExecutor final
    {
    public:
        static void record(RenderGraphInstance&, nri::CommandBuffer&, std::uint32_t slot) noexcept;

    private:
        static void recordHistoryInitialization(RenderGraphInstance&, nri::CommandBuffer&) noexcept;
    };
} // namespace lux::render::detail
