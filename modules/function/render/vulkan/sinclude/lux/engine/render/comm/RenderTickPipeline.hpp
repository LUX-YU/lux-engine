#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/renderer/FrameOrchestrator.hpp>
#include <lux/engine/function/render/client/core/FrameStamp.hpp>

#include <cstdint>
#include <vector>

namespace lux::render
{

    // (SceneViewBatch / SceneViewBatchItem 已随帧编排一并迁入 L4 的
    //  FrameOrchestrator.hpp —— 它们的词汇是场景与视图,与线协议无关;当初住在
    //  这里只是因为 tick 住在这里。本头 include 之,既有用法不变。)

    /// Provides one canonical "beginTick -> drainRequests" ordering.
    class LUX_FUNCTION_PUBLIC RenderTickPipeline{public : template <typename DrainFn> static bool runTick(
        FrameOrchestrator & orchestrator,
        FrameStamp& stamp,
        DrainFn&& drain_requests,
        uint32_t image_index_hint = 0
    ){stamp = orchestrator.beginTick(image_index_hint);
    return drain_requests();
}
}
;

} // namespace lux::render
