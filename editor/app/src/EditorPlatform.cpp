#include <lux/engine/editor/desktop/WindowInput.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <lux/engine/editor/detail/EditorImpl.hpp>
#include <optional>
namespace lux::editor
{
    void Editor::Impl::collectInput()
    {
        if (!window)
        {
            return;
        }
        lux::window::LuxWindow::pollEvents();
        input.sample(*window);
        const auto accepted = desktop::feedWindowInput(*root, input.snapshot());
        if (!accepted)
            fail({EEditorError::FRONTEND_FAILURE, "editor.ui.input", static_cast<std::uint64_t>(accepted.error())});
        if (!native_close && window->shouldClose())
        {
            std::fprintf(stderr, "[editor.exit] event=native-close-flag\n");
            native_close = true;
            exit_intent = true;
        }
    }

    void Editor::Impl::clearPlatformInput() noexcept
    {
        if (window)
        {
            root->closeInput();
        }
    }
    void Editor::Impl::cancelNativeClose() noexcept
    {
        native_close = false;
    }
} // namespace lux::editor
