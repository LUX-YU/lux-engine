#pragma once
#include <functional>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/EditorLayout.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lux::ui
{
    class Pane;
}
namespace lux::editor
{
    class EditorContext;
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

    private:
        friend class EditorContext;
        friend FrameworkResult<std::unique_ptr<ui::Pane>> createPane(EditorContext&, const PaneDescription&) noexcept;
        using FactoryRef = std::reference_wrapper<UiFactory>;
        [[nodiscard]] FrameworkResult<FactoryRef> resolveFactory(std::string_view) noexcept;
        void freeze() noexcept
        {
            frozen_ = true;
        }
        struct Entry final
        {
            std::string type;
            UiFactory factory;
        };
        std::vector<Entry> entries_;
        bool frozen_{};
    };
} // namespace lux::editor
