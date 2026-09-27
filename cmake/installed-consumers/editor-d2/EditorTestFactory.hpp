#pragma once
#include <lux/engine/editor/Editor.hpp>
#include "TestAssembly.hpp"
namespace lux::editor
{
    struct EditorTestFactory final
    {
        static EditorResult<std::unique_ptr<Editor>> create(EditorConfig config) noexcept
        { return Editor::create(std::move(config), &test::assembleProduct); }
        template<class Tool> static EditorResult<std::reference_wrapper<Tool>> show(Editor& editor)
        {
            std::string_view type;
            if constexpr (std::same_as<Tool, scene::SceneEditor>) type = scene::kSceneEditorType;
            else if constexpr (std::same_as<Tool, material::MaterialEditor>) type = material::kMaterialEditorType;
            else if constexpr (std::same_as<Tool, flowforge::FlowForgeEditor>) type = flowforge::kFlowForgeEditorType;
            if (auto* existing = editor.context().panes().findFirst(lux::ui::PaneTypeIdView{type}))
                return std::ref(static_cast<Tool&>(*existing));
            auto shown = editor.context().panes().create(lux::ui::PaneTypeIdView{type});
            if (!shown) return lux::cxx::unexpected(shown.error());
            return std::ref(static_cast<Tool&>(shown->get()));
        }
    };
}
