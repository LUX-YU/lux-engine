#pragma once

#include <lux/engine/editor/desktop/DesktopError.hpp>
#include <lux/engine/scene/RenderResources.hpp>
#include <lux/engine/scene/SceneInstanceId.hpp>
#include <lux/engine/ui/DrawData.hpp>
#include <lux/engine/ui/Root.hpp>
#include <chrono>

namespace lux::scene
{
    class SceneRuntime;
}

namespace lux::window
{
    class LuxWindow;
}

namespace lux::editor::desktop
{
    // Owns captured UI inputs and native output. SceneRuntime owns the formal UI scene.
    class Presentation final
    {
    public:
        [[nodiscard]] static DesktopResult<std::unique_ptr<Presentation>> create(
            lux::ui::Root&,
            process::ExecutionRuntime&,
            lux::scene::SceneRuntime&,
            render::RenderRuntime&,
            lux::scene::RenderResources&,
            window::LuxWindow* = nullptr
        ) noexcept;
        ~Presentation() noexcept;
        Presentation(const Presentation&) = delete;
        Presentation& operator=(const Presentation&) = delete;
        Presentation(Presentation&&) = delete;
        Presentation& operator=(Presentation&&) = delete;

        [[nodiscard]] lux::ui::FrameInfo frameInfo() noexcept;
        [[nodiscard]] std::chrono::steady_clock::time_point nextFrameTime() const noexcept;
        [[nodiscard]] lux::ui::DrawData* tryAcquireDrawData() noexcept;
        [[nodiscard]] lux::cxx::expected<void, lux::ui::ECaptureError>
        captureDrawData(const lux::ui::DrawData&) noexcept;
        [[nodiscard]] DesktopResult<void> applySceneInput() noexcept;
        void stopFrames() noexcept;
        [[nodiscard]] std::uint64_t capturedFrames() const noexcept;
        [[nodiscard]] lux::scene::SceneInstanceId sceneId() const noexcept;

    private:
        struct Impl;
        explicit Presentation(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
