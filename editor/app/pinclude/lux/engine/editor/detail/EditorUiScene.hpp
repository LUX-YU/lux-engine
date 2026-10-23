#pragma once

#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/editor/UiTargetImage.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/ui/DrawData.hpp>
#include <lux/engine/ui/FontAtlas.hpp>
#include <memory>

namespace lux::rendering
{
    class RenderExecution;
    struct FrameInput;
} // namespace lux::rendering

namespace lux::editor
{
    // One window's independent Render Scene. No empty Engine World, simulation, thread or frame ring.
    // All requests use the application's existing Main/Asset clients and execution resource.
    class EditorUiScene final
    {
    public:
        [[nodiscard]] static FrameworkResult<std::unique_ptr<EditorUiScene>> create(
            rendering::RenderExecution&,
            render::TargetDesc,
            render::SurfaceDescription,
            ui::FontAtlas
        ) noexcept;
        ~EditorUiScene() noexcept;
        EditorUiScene(const EditorUiScene&) = delete;
        EditorUiScene& operator=(const EditorUiScene&) = delete;

        // Record desired window state; zero/minimized extents disable presentation until restored.
        void setExtent(std::uint32_t width, std::uint32_t height, bool active) noexcept;
        // Cold image-set change. Keep removed Targets alive until imagesApplied() becomes true.
        [[nodiscard]] FrameworkResult<void> setTargetImages(std::vector<UiTargetImage>) noexcept;
        [[nodiscard]] FrameworkResult<rdesc::UiImageToken> addTargetImage(
            render::TargetId,
            render::TargetDesc
        ) noexcept;
        void removeTargetImage(rdesc::UiImageToken) noexcept;
        [[nodiscard]] bool imagesApplied() const noexcept;
        // Starts a finite reconfiguration/close transaction. Application keeps driving complete frames.
        [[nodiscard]] FrameworkResult<void> maintain() noexcept;
        [[nodiscard]] FrameworkResult<bool> outputReady() const noexcept;
        [[nodiscard]] ui::DrawData& captureBuffer(rendering::FrameInput&) noexcept;
        [[nodiscard]] FrameworkResult<void> appendFrameCommands(rendering::FrameInput&) noexcept;
        void stopFrames() noexcept;
        // Only protocol work has settled. Native-window lifetime ends at successful RemoveTarget
        // or after RenderExecution shutdown; an error never claims an unavailable response succeeded.
        [[nodiscard]] bool settled() const noexcept;

    private:
        struct Impl;
        explicit EditorUiScene(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
