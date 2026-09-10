#pragma once
// Non-installed diagnostics. Faults are attached to an actual owning packet before real queue admission.
#include <lux/engine/editor/rendering/EditorFramePacket.hpp>
namespace lux::editor::rendering { class EditorRenderer; }
extern "C" LUX_EDITOR_RENDERING_PUBLIC void lux_er1_renderer_allocation_fail_after(std::size_t) noexcept;
extern "C" LUX_EDITOR_RENDERING_PUBLIC std::size_t lux_er1_renderer_allocation_disarm() noexcept;
namespace lux::editor::rendering::detail
{
    struct LUX_EDITOR_RENDERING_PUBLIC RendererTestAccess final
    {
        static RenderResult<void> failRecord(EditorFramePacket &, lux::render::RenderError) noexcept;
        // Observe the real admitted request, without delaying or replacing its reply.
        static std::uint64_t inFlightResize(const RenderView &) noexcept;
        static void useSceneForNextView(lux::render::RenderSceneId) noexcept;
        static RenderResult<void> pauseConsumer(EditorRenderer &, bool) noexcept;
        static bool consumerPaused(const EditorRenderer &) noexcept;
    };
}
