#pragma once

#include <lux/engine/function/render/client/RenderProgram.hpp>
#include <lux/engine/scene/RenderSyncPipeline.hpp>

namespace lux::scene::detail
{
    // Binding owns this stable storage. Scene extraction borrows it on Main.
    // A prepared update is immutable until accepted or reliably retired.
    struct RenderSyncStorage final
    {
        render::RenderProgram<> update;
        RenderSyncStatistics statistics;
        bool prepared{};
        bool producer_closed{};
        bool stopped{};
    };
} // namespace lux::scene::detail
