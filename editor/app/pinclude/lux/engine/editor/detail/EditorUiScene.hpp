#pragma once
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/scene/RenderResourceTypes.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/ui/DrawData.hpp>
#include <memory>
#include <optional>
#include <span>

namespace lux::engine
{
    class EngineContext;
}
namespace lux::editor
{
    // UI scene/input transport only. No window, project, root or Pane is borrowed.
    class EditorUiScene final
    {
    public:
        [[nodiscard]] static FrameworkResult<std::unique_ptr<EditorUiScene>> create(
            engine::EngineContext&,
            std::vector<std::byte> render_configuration,
            std::optional<scene::ViewConfig> output = {}
        ) noexcept;
        ~EditorUiScene() noexcept;
        EditorUiScene(const EditorUiScene&) = delete;
        EditorUiScene& operator=(const EditorUiScene&) = delete;
        EditorUiScene(EditorUiScene&&) = delete;
        EditorUiScene& operator=(EditorUiScene&&) = delete;
        void setExtent(render::PixelExtent) noexcept;
        [[nodiscard]] bool outputReady() noexcept;
        [[nodiscard]] ui::DrawData* acquireDrawData() noexcept;
        [[nodiscard]] cxx::expected<void, ui::ECaptureError> captureDrawData(const ui::DrawData&) noexcept;
        [[nodiscard]] FrameworkResult<void> publishFrame() noexcept;
        void stopFrames() noexcept;
        [[nodiscard]] scene::SceneInstanceId sceneId() const noexcept;

    private:
        struct Impl;
        explicit EditorUiScene(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
