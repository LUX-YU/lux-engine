#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/FrameworkErrors.hpp>
#include <lux/engine/editor/detail/WindowInput.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>

namespace lux::editor
{
    EditorWindow::EditorWindow(const window::InitParameter& config) : LuxWindow(config) {}
    EditorWindow::~EditorWindow() = default;
    FrameworkResult<std::unique_ptr<EditorWindow>> EditorWindow::create(const window::InitParameter& config) noexcept
    {
        if (auto registered = registerFrameworkErrors(); !registered)
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
        return window;
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
