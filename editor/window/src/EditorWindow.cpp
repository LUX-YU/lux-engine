#include <lux/engine/ui/Root.hpp>
#include <lux/engine/editor/EditorWindow.hpp>
#include <lux/engine/editor/WindowInput.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>

namespace lux::editor
{
    EditorWindow::EditorWindow(const window::InitParameter& config) : LuxWindow(config) {}
    EditorWindow::~EditorWindow() = default;
    FrameworkResult<std::unique_ptr<EditorWindow>> EditorWindow::create(
        const window::InitParameter& config
    ) noexcept
    {
        static window::GlfwRuntime runtime;
        if (!runtime.valid())
        {
            return cxx::unexpected(error::makeError(
                {"lux.editor.glfw_initialization_failed", "GLFW initialization failed", error::ERecovery::PERMANENT}
            ));
        }
        auto window = std::unique_ptr<EditorWindow>{new EditorWindow(config)};
        if (!window->isInitialized())
        {
            return cxx::unexpected(error::makeError(
                {"lux.editor.native_window_creation_failed",
                 "Native window creation failed: code {0}",
                 error::ERecovery::PERMANENT,
                 {error::EArgument::UNSIGNED}},
                {static_cast<std::uint64_t>(window->initError())}
            ));
        }
        auto root = ui::Root::create();
        if (!root)
        {
            return cxx::unexpected(error::makeError(
                {"lux.editor.root_initialization_failed",
                 "Root initialization failed: code {0}",
                 error::ERecovery::PERMANENT,
                 {error::EArgument::UNSIGNED}},
                {static_cast<std::uint64_t>(root.error())}
            ));
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
            return cxx::unexpected(error::makeError(
                {"lux.editor.native_input_delivery_failed",
                 "Native input delivery failed: code {0}",
                 error::ERecovery::PERMANENT,
                 {error::EArgument::UNSIGNED}},
                {static_cast<std::uint64_t>(fed.error())}
            ));
        }
        return {};
    }
} // namespace lux::editor
