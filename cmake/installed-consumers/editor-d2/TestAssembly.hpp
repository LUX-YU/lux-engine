#pragma once
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/editor/flowforge/FlowForgeEditor.hpp>
#include <lux/engine/editor/project/ProjectPane.hpp>
#include <lux/engine/editor/settings/SettingPane.hpp>
#include <lux/engine/editor/ui/TaskPane.hpp>
#include <lux/engine/editor/launcher/ProjectCreationPane.hpp>
#include <lux/engine/object/ObjectEvent.hpp>

namespace lux::editor::test
{
    namespace
    {
        template<class Tool> struct ToolType;
        template<> struct ToolType<scene::SceneEditor> { static constexpr auto name = scene::kSceneEditorType; };
        template<> struct ToolType<material::MaterialEditor> { static constexpr auto name = material::kMaterialEditorType; };
        template<> struct ToolType<flowforge::FlowForgeEditor> { static constexpr auto name = flowforge::kFlowForgeEditorType; };
        template<> struct ToolType<ProjectPane> { static constexpr auto name = "lux.editor.project"; };
        template<> struct ToolType<SettingPane> { static constexpr auto name = "lux.editor.settings"; };
        template<> struct ToolType<ui::TaskPane> { static constexpr auto name = "lux.editor.tasks"; };
    }
    namespace
    {
        template<class Tool> PaneRegistration::CreateResult createTool(PaneManager& panes) noexcept
        {
            auto created = Tool::create(panes.root(), panes.makeId(), panes.context());
            if (!created)
                return lux::cxx::unexpected(created.error());
            return panes.adopt(std::move(*created));
        }
        template<class Tool> PaneRegistration::CreateResult openTool(PaneManager& panes, asset::AssetId asset) noexcept
        {
            for (const auto& pane : panes.panes())
            {
                if (pane->type().view() != lux::ui::PaneTypeIdView{ToolType<Tool>::name})
                    continue;
                AssetEditorQuery query{asset};
                if (object::sendEvent(*pane, query) && query.matches)
                    return std::ref(*pane);
            }
            auto created = Tool::create(panes.root(), panes.makeId(), panes.context());
            if (!created)
                return lux::cxx::unexpected(created.error());
            if (auto opened = (*created)->openAsset(asset); !opened)
                return lux::cxx::unexpected(opened.error());
            return panes.adopt(std::move(*created));
        }
        template<class Tool> PaneRegistration::CreateResult singleton(PaneManager& panes) noexcept
        {
            if (auto* existing = panes.findFirst(lux::ui::PaneTypeIdView{ToolType<Tool>::name}))
                return std::ref(*existing);
            EditorResult<void> status;
            auto created = std::make_unique<Tool>(panes.root(), panes.context(), status);
            if (!status)
                return lux::cxx::unexpected(status.error());
            return panes.adopt(std::move(created));
        }
    }
    inline EditorResult<void> assembleProduct(lux::ui::Root&, EditorContext& context) noexcept
    {
        std::vector<CommandRegistration> commands(context.commands().begin(), context.commands().end());
        CommandRegistration startup;
        startup.id = lux::ui::CommandId{"lux.product.default"};
        startup.label = "Open default asset";
        startup.invoke = [](EditorContext& context, lux::ui::Command& command) -> EditorResult<void> {
            command.enabled = true;
            if (command.phase == lux::ui::ECommandPhase::QUERY)
                return {};
            const auto& manifest = context.project().manifest();
            const auto found = std::ranges::find(manifest.assets, manifest.default_scene, &ProjectAssetEntry::source_path);
            if (found == manifest.assets.end())
                return {};
            auto opened = context.openAsset(found->id);
            return opened ? EditorResult<void>{} : lux::cxx::unexpected(opened.error());
        };
        commands.push_back(std::move(startup));
        if (auto registered = context.setCommands(std::move(commands)); !registered)
            return registered;

        std::vector<PaneRegistration> panes(context.panes().registrations().begin(), context.panes().registrations().end());
        panes.push_back({lux::ui::PaneTypeId{scene::kSceneEditorType}, "Scene Editor", &createTool<scene::SceneEditor>});
        panes.push_back({lux::ui::PaneTypeId{material::kMaterialEditorType}, "Material Editor", &createTool<material::MaterialEditor>});
        panes.push_back({lux::ui::PaneTypeId{flowforge::kFlowForgeEditorType}, "FlowForge Editor", &createTool<flowforge::FlowForgeEditor>});
        panes.push_back({lux::ui::PaneTypeId{"lux.editor.project"}, "Project", &singleton<ProjectPane>});
        panes.push_back({lux::ui::PaneTypeId{"lux.editor.settings"}, "Settings", &singleton<SettingPane>});
        panes.push_back({lux::ui::PaneTypeId{"lux.editor.tasks"}, "Background tasks", &singleton<ui::TaskPane>});
        panes.push_back({lux::ui::PaneTypeId{"lux.editor.project.creation"}, "New project (new Editor)",
            [](PaneManager& manager) noexcept -> PaneRegistration::CreateResult {
                if (auto* existing = manager.findFirst(lux::ui::PaneTypeIdView{"lux.editor.project.creation"}))
                 {
                     if (!static_cast<ProjectCreationPane&>(*existing).closed())
                         return std::ref(*existing);
                     const auto id = existing->id();
                     static_cast<void>(manager.erase(id.view()));
                 }
                 auto created = ProjectCreationPane::create(manager.root(), manager.context().execution(),
                                                           manager.context().installation());
                if (!created)
                    return lux::cxx::unexpected(created.error());
                return manager.adopt(std::move(*created));
            }});
        std::vector<AssetEditorRegistration> assets(context.assetEditors().begin(), context.assetEditors().end());
        assets.push_back({lux::ui::PaneTypeId{scene::kSceneEditorType},
            [](const ProjectAssetEntry& entry) noexcept { return entry.kind == EProjectAssetKind::SCENE; },
            &openTool<scene::SceneEditor>});
        assets.push_back({lux::ui::PaneTypeId{material::kMaterialEditorType},
            [](const ProjectAssetEntry& entry) noexcept { return entry.kind == EProjectAssetKind::MATERIAL_GRAPH; },
            &openTool<material::MaterialEditor>});
        assets.push_back({lux::ui::PaneTypeId{flowforge::kFlowForgeEditorType},
            [](const ProjectAssetEntry& entry) noexcept { return entry.kind == EProjectAssetKind::FLOW_GRAPH; },
            &openTool<flowforge::FlowForgeEditor>});
        if (auto registered = context.panes().setRegistrations(std::move(panes)); !registered)
            return registered;
        if (auto registered = context.setAssetEditors(std::move(assets)); !registered)
            return registered;
        const auto project = context.panes().create(lux::ui::PaneTypeIdView{"lux.editor.project"});
        return project ? EditorResult<void>{} : lux::cxx::unexpected(project.error());
    }
}
