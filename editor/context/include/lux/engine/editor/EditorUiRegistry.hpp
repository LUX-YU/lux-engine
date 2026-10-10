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
    class EditorComposition;
    using UiFactory = cxx::move_only_function<
        FrameworkResult<std::unique_ptr<ui::Pane>>(EditorContext&, const PaneDescription&) noexcept>;
    class EditorUiRegistry final
    {
    public:
        class Factory final
        {
        public:
            Factory(Factory&&) noexcept = default;
            Factory& operator=(Factory&&) noexcept = default;

        private:
            friend class EditorComposition;
            friend class EditorUiRegistry;
            friend FrameworkResult<std::unique_ptr<ui::Pane>> createPane(EditorContext&, const PaneDescription&) noexcept;
            Factory(std::string type, UiFactory create) noexcept : type_(std::move(type)), create_(std::move(create)) {}

            std::string type_;
            mutable UiFactory create_;
        };
        using FactoryRef = std::reference_wrapper<const Factory>;
        ~EditorUiRegistry();
        EditorUiRegistry(const EditorUiRegistry&) = delete;
        EditorUiRegistry& operator=(const EditorUiRegistry&) = delete;
        EditorUiRegistry(EditorUiRegistry&&) = delete;
        EditorUiRegistry& operator=(EditorUiRegistry&&) = delete;
        [[nodiscard]] FrameworkResult<FactoryRef> resolveFactory(std::string_view) const noexcept;

    private:
        friend class EditorContext;
        friend class EditorComposition;
        explicit EditorUiRegistry(std::vector<Factory>) noexcept;
        std::vector<Factory> entries_;
    };
} // namespace lux::editor
