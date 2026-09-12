#pragma once
#include <lux/engine/editor/scene/visibility.h>
#include <lux/engine/scene/RenderRuntime.hpp>
#include <lux/engine/function/render/client/features/view_camera/ViewCameraOperation.hpp>
#include <lux/engine/ui/TextureHandle.hpp>

namespace lux::editor::workbench
{
    struct SceneViewDiagnostics final
    {
        std::uint64_t frames{}, slot_mask{}, descriptors_created{}, descriptors_retired{}, texture_misses{};
        std::uint64_t render_events{}, dropped_events{};
    };
    struct SceneViewFrame final
    {
        render::ViewCameraUpdatePayload camera{};
        render::RenderTargetId target{};
        lux::ui::TextureHandle texture{};
        std::shared_ptr<const void> cpu_lease;
    };

    class LUX_EDITOR_SCENE_PUBLIC SceneViewRenderPort
    {
    public:
        virtual ~SceneViewRenderPort();
        virtual lux::scene::RenderRuntime& runtime() noexcept = 0;
        virtual void pump() = 0;
        virtual bool framePending() const noexcept = 0;
        virtual void deferNewFrames(bool defer) noexcept = 0;
        virtual SceneViewDiagnostics diagnostics() const noexcept = 0;
        virtual void drainViewFrames() = 0;
        virtual bool stopping() const noexcept = 0;
        virtual void setViewFrame(const SceneViewFrame& frame) noexcept = 0;
    };
}
