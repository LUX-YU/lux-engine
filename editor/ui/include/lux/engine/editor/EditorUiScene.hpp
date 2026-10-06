#pragma once
#include <lux/engine/editor/FrameworkError.hpp>
#include <lux/engine/scene/RenderResourceTypes.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/ui/DrawData.hpp>
#include <memory>
#include <optional>
#include <span>

namespace lux::process
{
    class ExecutionRuntime;
}
namespace lux::scene
{
    class SceneRuntime;
    class RenderResources;
} // namespace lux::scene
namespace lux::render
{
    class RenderRuntime;
}
namespace lux::editor
{
    // UI scene/input transport only. No window, project, root or Pane is borrowed.
    class EditorUiScene final
    {
    public:
        [[nodiscard]] static FrameworkResult<std::unique_ptr<EditorUiScene>> create(
            process::ExecutionRuntime&,
            scene::SceneRuntime&,
            render::RenderRuntime&,
            scene::RenderResources&,
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
        [[nodiscard]] FrameworkResult<void> publishInput() noexcept;
        void stopFrames() noexcept;
        [[nodiscard]] std::uint64_t capturedFrames() const noexcept;
        [[nodiscard]] scene::SceneInstanceId sceneId() const noexcept;
        [[nodiscard]] bool hasWritableFrame() const noexcept;

    private:
        struct Impl;
        explicit EditorUiScene(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
