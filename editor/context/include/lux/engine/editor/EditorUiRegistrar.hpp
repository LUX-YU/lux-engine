#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/FrameworkError.hpp>
#include <memory>
#include <vector>

namespace lux::ui
{
    class Pane;
}
namespace lux::editor
{
    class EditorContext;
    struct PaneDescription final
    {
        std::string type;
        std::string name;
        std::string title;
    };
    using EditorLayout = std::vector<PaneDescription>;
    using UiFactory =
        cxx::move_only_function<FrameworkResult<std::unique_ptr<ui::Pane>>(EditorContext&, const PaneDescription&)>;

    class EditorUiRegistrar final
    {
    public:
        EditorUiRegistrar() = default;
        ~EditorUiRegistrar();
        EditorUiRegistrar(const EditorUiRegistrar&) = delete;
        EditorUiRegistrar& operator=(const EditorUiRegistrar&) = delete;
        EditorUiRegistrar(EditorUiRegistrar&&) = delete;
        EditorUiRegistrar& operator=(EditorUiRegistrar&&) = delete;
        [[nodiscard]] FrameworkResult<void> registerFactory(std::string, UiFactory) noexcept;
        // Defined in lux_editor_ui: invocation/destruction need the complete Pane type.
        [[nodiscard]] FrameworkResult<std::unique_ptr<ui::Pane>> create(EditorContext&, const PaneDescription&) noexcept;
        void freeze() noexcept
        {
            frozen_ = true;
        }

    private:
        [[nodiscard]] FrameworkResult<std::reference_wrapper<UiFactory>> findFactory(std::string_view) noexcept;
        struct Entry final
        {
            std::string type;
            UiFactory factory;
        };
        std::vector<Entry> entries_;
        bool frozen_{};
    };
} // namespace lux::editor
