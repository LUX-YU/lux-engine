#include <lux/engine/editor/EditorUiErrors.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/detail/WindowInput.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>

namespace lux::editor
{
    EditorWindow::EditorWindow(const window::InitParameter& config) : LuxWindow(config) {}

    EditorWindow::~EditorWindow() = default;

    FrameworkResult<std::unique_ptr<EditorWindow>> EditorWindow::create(const window::InitParameter& config) noexcept
    {
        if (auto registered = registerEditorUiErrors(); !registered)
        {
            return cxx::unexpected(registered.error());
        }
        static window::GlfwRuntime runtime;
        if (!runtime.valid())
        {
            return cxx::unexpected(error::Error{Errors::EditorGlfwInitializationFailed, {}});
        }
        auto window = std::unique_ptr<EditorWindow>{new EditorWindow(config)};
        if (!window->isInitialized())
        {
            return cxx::unexpected(error::Error{
                Errors::EditorNativeWindowCreationFailed,
                {static_cast<std::uint64_t>(window->initError())}
            });
        }
        auto root = ui::Root::create();
        if (!root)
        {
            return cxx::unexpected(
                error::Error{Errors::EditorRootInitializationFailed, {static_cast<std::uint64_t>(root.error())}}
            );
        }
        window->root_ = std::move(*root);
        window->root_->bindWindow(window.get());
        window->size(window->metrics_.width, window->metrics_.height);
        window->framebufferSize(window->metrics_.framebuffer_width, window->metrics_.framebuffer_height);
        window->metrics_.minimized = window->minimized();
        window->on_resize = [owner = window.get()](const window::WindowResizeEvent& event) noexcept
        {
            auto& metrics = owner->metrics_;
            const bool changed = metrics.width != event.width || metrics.height != event.height;
            if (changed)
            {
                metrics.width = event.width;
                metrics.height = event.height;
                owner->metricsChanged();
            }
        };
        window->on_framebuffer_resize = [owner = window.get()](const window::FramebufferResizeEvent& event) noexcept
        {
            auto& metrics = owner->metrics_;
            const bool changed = metrics.framebuffer_width != event.width || metrics.framebuffer_height != event.height;
            if (changed)
            {
                metrics.framebuffer_width = event.width;
                metrics.framebuffer_height = event.height;
                owner->metricsChanged();
            }
        };
        window->on_minimized = [owner = window.get()](const window::WindowMinimizedEvent& event) noexcept
        {
            if (owner->metrics_.minimized != event.minimized)
            {
                owner->metrics_.minimized = event.minimized;
                owner->metricsChanged();
            }
        };
        return window;
    }

    void EditorWindow::metricsChanged() noexcept
    {
        if (++metrics_.revision == 0)
        {
            std::terminate();
        }
        wakeEvents();
    }

    ui::Root& EditorWindow::uiRoot() noexcept
    {
        return *root_;
    }

    FrameworkResult<void> EditorWindow::sampleInput() noexcept
    {
        input_.sample(*this);
        auto fed = feedWindowInput(*root_, input_.snapshot());
        if (!fed)
        {
            return cxx::unexpected(
                error::Error{Errors::EditorNativeInputDeliveryFailed, {static_cast<std::uint64_t>(fed.error())}}
            );
        }
        return {};
    }
} // namespace lux::editor
