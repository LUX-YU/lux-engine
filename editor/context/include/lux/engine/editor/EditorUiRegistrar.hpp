#pragma once
#include <functional>
#include <lux/cxx/core/StableNameId.hpp>
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
    struct UiTypeTag final
    {
    };
    using UiTypeId = cxx::StableNameId<UiTypeTag>;
    struct UiDescription final
    {
        UiTypeId type;
        std::string instance;
        std::string title;
    };
    using EditorLayout = std::vector<UiDescription>;
    using UiFactory =
        cxx::move_only_function<FrameworkResult<std::unique_ptr<ui::Pane>>(EditorContext&, const UiDescription&)>;

    class EditorUiRegistrar final
    {
    public:
        EditorUiRegistrar() = default;
        ~EditorUiRegistrar();
        EditorUiRegistrar(const EditorUiRegistrar&) = delete;
        EditorUiRegistrar& operator=(const EditorUiRegistrar&) = delete;
        EditorUiRegistrar(EditorUiRegistrar&&) = delete;
        EditorUiRegistrar& operator=(EditorUiRegistrar&&) = delete;
        [[nodiscard]] FrameworkResult<void> registerFactory(UiTypeId, UiFactory) noexcept;
        // Defined in lux_editor_ui: invocation/destruction need the complete Pane type.
        [[nodiscard]] FrameworkResult<std::unique_ptr<ui::Pane>> create(EditorContext&, const UiDescription&) noexcept;
        void freeze() noexcept
        {
            frozen_ = true;
        }

    private:
        [[nodiscard]] FrameworkResult<std::reference_wrapper<UiFactory>> findFactory(const UiTypeId&) noexcept;
        struct Entry final
        {
            UiTypeId type;
            UiFactory factory;
        };
        std::vector<Entry> entries_;
        bool frozen_{};
    };
} // namespace lux::editor
