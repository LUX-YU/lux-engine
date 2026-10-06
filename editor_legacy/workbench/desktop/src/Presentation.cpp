#include <lux/engine/editor/desktop/Presentation.hpp>
#include <lux/engine/editor/desktop/WindowOutput.hpp>
#include <lux/engine/editor/EditorUiScene.hpp>
#include <lux/engine/ui/rendering/RenderFeature.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <algorithm>

namespace lux::editor::desktop
{
    // Legacy scheduling only. Frame storage, pinning, publication and retirement have one implementation.
    struct Presentation::Impl final
    {
        window::LuxWindow* window{};
        std::unique_ptr<EditorUiScene> scene;
        std::chrono::steady_clock::time_point last{std::chrono::steady_clock::now()}, next{};
        bool stopped{};
    };
    DesktopResult<std::unique_ptr<Presentation>> Presentation::create(
        ui::Root& root, process::ExecutionRuntime& execution, lux::scene::SceneRuntime& scenes,
        render::RenderRuntime& runtime, lux::scene::RenderResources& resources, window::LuxWindow* window
    ) noexcept
    {
        auto configuration = ui::makeRenderConfiguration(root);
        if (!configuration)
            return cxx::unexpected(DesktopFailure{"ui.configuration", configuration.error()});
        std::optional<lux::scene::ViewConfig> output;
        if (window)
        {
            auto surface = windowOutput(*window);
            if (!surface)
                return cxx::unexpected(surface.error());
            std::uint32_t width{}, height{};
            window->framebufferSize(width, height);
            output = lux::scene::ViewConfig{.extent = {width, height}, .output = *surface};
        }
        auto scene = EditorUiScene::create(execution, scenes, runtime, resources, std::move(*configuration), output);
        if (!scene)
            return cxx::unexpected(DesktopFailure{"ui.scene", std::move(scene.error())});
        auto impl = std::make_unique<Impl>();
        impl->window = window;
        impl->scene = std::move(*scene);
        return std::unique_ptr<Presentation>{new Presentation(std::move(impl))};
    }
    Presentation::Presentation(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    Presentation::~Presentation() noexcept = default;
    ui::FrameInfo Presentation::frameInfo() noexcept
    {
        if (!impl_->window || impl_->stopped)
            return {};
        std::uint32_t width{}, height{}, pixels_x{}, pixels_y{};
        impl_->window->size(width, height);
        impl_->window->framebufferSize(pixels_x, pixels_y);
        impl_->scene->setExtent({pixels_x, pixels_y});
        const bool has_extent = width && height && pixels_x && pixels_y;
        if (!has_extent || impl_->window->minimized() || !impl_->scene->outputReady())
            return {};
        const auto elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - impl_->last).count();
        return {{float(width), float(height)}, std::clamp(elapsed, 0.001F, 0.1F),
                {float(pixels_x) / width, float(pixels_y) / height}};
    }
    std::chrono::steady_clock::time_point Presentation::nextFrameTime() const noexcept
    {
        return !impl_->stopped && impl_->scene->hasWritableFrame() ? impl_->next :
            std::chrono::steady_clock::time_point::max();
    }
    ui::DrawData* Presentation::tryAcquireDrawData() noexcept
    {
        return std::chrono::steady_clock::now() < impl_->next ? nullptr : impl_->scene->acquireDrawData();
    }
    cxx::expected<void, ui::ECaptureError> Presentation::captureDrawData(const ui::DrawData& data) noexcept
    {
        auto result = impl_->scene->captureDrawData(data);
        if (result)
        {
            impl_->last = std::chrono::steady_clock::now();
            impl_->next = impl_->last + std::chrono::milliseconds(16);
        }
        return result;
    }
    DesktopResult<void> Presentation::applySceneInput() noexcept
    {
        auto result = impl_->scene->publishInput();
        if (!result)
            return cxx::unexpected(DesktopFailure{"ui.input", std::move(result.error())});
        return {};
    }
    void Presentation::stopFrames() noexcept { impl_->stopped = true; impl_->scene->stopFrames(); }
    std::uint64_t Presentation::capturedFrames() const noexcept { return impl_->scene->capturedFrames(); }
    lux::scene::SceneInstanceId Presentation::sceneId() const noexcept { return impl_->scene->sceneId(); }
}
