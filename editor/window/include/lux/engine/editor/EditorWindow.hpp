#pragma once
#include <lux/engine/editor/FrameworkError.hpp>
#include <lux/engine/input/Input.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/window/LuxWindow.hpp>

namespace lux::editor
{
    class EditorUIRoot;
    class EditorWindow final : public window::LuxWindow
    {
    public:
        [[nodiscard]] static FrameworkResult<std::unique_ptr<EditorWindow>> create(
            const window::InitParameter&,
            object::ObjectDispatcherRef
        ) noexcept;
        ~EditorWindow() override;
        EditorWindow(const EditorWindow&) = delete;
        EditorWindow& operator=(const EditorWindow&) = delete;
        EditorWindow(EditorWindow&&) = delete;
        EditorWindow& operator=(EditorWindow&&) = delete;
        [[nodiscard]] EditorUIRoot& uiRoot() noexcept;
        [[nodiscard]] input::Input& input() noexcept
        {
            return input_;
        }
        [[nodiscard]] FrameworkResult<void> sampleInput() noexcept;

    private:
        explicit EditorWindow(const window::InitParameter&);
        input::Input input_;
        std::unique_ptr<EditorUIRoot> root_;
    };
} // namespace lux::editor
