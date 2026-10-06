#include <lux/engine/editor/EditorUIRoot.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/WindowInput.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>

namespace lux::editor
{
    EditorWindow::EditorWindow(const window::InitParameter& config) : LuxWindow(config) {}
    EditorWindow::~EditorWindow() = default;
    FrameworkResult<std::unique_ptr<EditorWindow>> EditorWindow::create(
        const window::InitParameter& config,
        object::ObjectDispatcherRef dispatcher
    ) noexcept
    {
        static window::GlfwRuntime runtime;
        if (!runtime.valid())
        {
            return cxx::unexpected(FrameworkFailure{EFrameworkError::WINDOW, "GLFW initialization failed"});
        }
        auto window = std::unique_ptr<EditorWindow>{new EditorWindow(config)};
        if (!window->isInitialized())
        {
            return cxx::unexpected(FrameworkFailure{
                EFrameworkError::WINDOW,
                "Native window creation failed",
                static_cast<std::uint64_t>(window->initError())
            });
        }
        auto root = EditorUIRoot::create(std::move(dispatcher));
        if (!root)
        {
            return cxx::unexpected(std::move(root.error()));
        }
        window->root_ = std::move(*root);
        window->root_->bindWindow(window.get());
        return window;
    }
    EditorUIRoot& EditorWindow::uiRoot() noexcept
    {
        return *root_;
    }
    FrameworkResult<void> EditorWindow::sampleInput() noexcept
    {
        input_.sample(*this);
        auto fed = feedWindowInput(*root_, input_.snapshot());
        if (!fed)
        {
            return cxx::unexpected(FrameworkFailure{
                EFrameworkError::UI,
                "Native input delivery failed",
                static_cast<std::uint64_t>(fed.error())
            });
        }
        return {};
    }
} // namespace lux::editor
