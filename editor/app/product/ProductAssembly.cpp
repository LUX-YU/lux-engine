#include <lux/engine/editor/project/ProjectManifest.hpp>
#include "ProductAssembly.hpp"
#include "ProductCommands.hpp"
#include <algorithm>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/material/MaterialEditor.hpp>
#include <lux/engine/editor/flowforge/FlowForgeEditor.hpp>
#include <lux/engine/editor/project/ProjectPane.hpp>
#include <lux/engine/editor/settings/SettingPane.hpp>
#include <lux/engine/editor/ui/TaskPane.hpp>
#include <lux/engine/editor/launcher/ProjectCreationPane.hpp>
#include <lux/engine/object/ObjectEvent.hpp>

namespace lux::editor
{
    namespace
    {
        template <class Tool> struct ToolType;
        template <> struct ToolType<scene::SceneEditor>
        {
            static constexpr auto name = scene::kSceneEditorType;
        };
        template <> struct ToolType<material::MaterialEditor>
        {
            static constexpr auto name = material::kMaterialEditorType;
        };
        template <> struct ToolType<flowforge::FlowForgeEditor>
        {
            static constexpr auto name = flowforge::kFlowForgeEditorType;
        };
        template <> struct ToolType<ProjectPane>
        {
            static constexpr auto name = "lux.editor.project";
        };
        template <> struct ToolType<SettingPane>
        {
            static constexpr auto name = "lux.editor.settings";
        };
        template <> struct ToolType<ui::TaskPane>
        {
            static constexpr auto name = "lux.editor.tasks";
        };
    }
    namespace
    {
        template <class Tool> PaneRegistration::CreateResult createTool(PaneManager& panes) noexcept
        {
            auto created = Tool::create(panes.root(), panes.makeId(), panes.context());
            if (!created)
                return lux::cxx::unexpected(created.error());
            return panes.adopt(std::move(*created));
        }
        template <class Tool> PaneRegistration::CreateResult openTool(PaneManager& panes, asset::AssetId asset) noexcept
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
        template <class Tool> PaneRegistration::CreateResult singleton(PaneManager& panes) noexcept
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
    namespace
    {
        template <class Tool>
        PaneRegistration::CreateResult restoreTool(PaneManager& panes, const PaneState& state) noexcept
        {
            asset::AssetId asset;
            if (!state.payload.empty())
            {
                if (!state.payload.starts_with("v1:"))
                    return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "workspace.asset.version"}
                    );
                const auto parsed = uuids::uuid::from_string(state.payload.substr(3));
                if (!parsed)
                    return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "workspace.asset"});
                asset = asset::AssetId{*parsed};
                for (const auto& pane : panes.panes())
                {
                    if (pane->type() != state.type)
                        continue;
                    AssetEditorQuery query{asset};
                    if (object::sendEvent(*pane, query) && query.matches)
                        return std::ref(*pane);
                }
            }
            else if (auto* existing = panes.find(state.id.view()); existing && existing->type() == state.type)
                return std::ref(*existing);
            const auto id = panes.find(state.id.view()) ? panes.makeId() : state.id;
            auto created = Tool::create(panes.root(), id, panes.context());
            if (!created)
                return lux::cxx::unexpected(created.error());
            if (!asset.isNull())
                if (auto opened = (*created)->openAsset(asset); !opened)
                    return lux::cxx::unexpected(opened.error());
            return panes.adopt(std::move(*created));
        }
        template <class Tool> void persistentTool(PaneRegistration& entry)
        {
            entry.capture = [](PaneManager& panes, const lux::ui::Pane& pane) noexcept -> EditorResult<std::string> {
                const auto& tool = static_cast<const Tool&>(pane);
                const auto asset = tool.assetId();
                return !panes.context().project().asset(asset) ? std::string{} : "v1:" + uuids::to_string(asset.uuid());
            };
            entry.restore = &restoreTool<Tool>;
        }
        template <class Tool> void persistentSingleton(PaneRegistration& entry)
        {
            entry.capture = [](PaneManager&, const lux::ui::Pane&) noexcept -> EditorResult<std::string> {
                return std::string{};
            };
            entry.restore = [](PaneManager& panes, const PaneState&) noexcept { return singleton<Tool>(panes); };
        }
    }
    EditorResult<void> assembleProduct(lux::ui::Root&, EditorContext& context) noexcept
    {
        std::vector<PaneRegistration> panes(
            context.panes().registrations().begin(),
            context.panes().registrations().end()
        );
        panes.push_back({lux::ui::PaneTypeId{scene::kSceneEditorType}, "Scene Editor", &createTool<scene::SceneEditor>}
        );
        panes.push_back(
            {lux::ui::PaneTypeId{material::kMaterialEditorType},
             "Material Editor",
             &createTool<material::MaterialEditor>}
        );
        panes.push_back(
            {lux::ui::PaneTypeId{flowforge::kFlowForgeEditorType},
             "FlowForge Editor",
             &createTool<flowforge::FlowForgeEditor>}
        );
        panes.push_back({lux::ui::PaneTypeId{"lux.editor.project"}, "Project", &singleton<ProjectPane>});
        panes.push_back({lux::ui::PaneTypeId{"lux.editor.settings"}, "Settings", &singleton<SettingPane>});
        panes.push_back({lux::ui::PaneTypeId{"lux.editor.tasks"}, "Background tasks", &singleton<ui::TaskPane>});
        panes.push_back(
            {lux::ui::PaneTypeId{"lux.editor.project.creation"},
             "New project (new Editor)",
             [](PaneManager& manager) noexcept -> PaneRegistration::CreateResult {
                 if (auto* existing = manager.findFirst(lux::ui::PaneTypeIdView{"lux.editor.project.creation"}))
                 {
                     if (!static_cast<ProjectCreationPane&>(*existing).closed())
                         return std::ref(*existing);
                     const auto id = existing->id();
                     static_cast<void>(manager.erase(id.view()));
                 }
                 auto created = ProjectCreationPane::create(
                     manager.root(),
                     manager.context().execution(),
                     manager.context().installation()
                 );
                 if (!created)
                     return lux::cxx::unexpected(created.error());
                 return manager.adopt(std::move(*created));
             }}
        );
        for (auto& entry : panes)
        {
            if (entry.type.view() == lux::ui::PaneTypeIdView{scene::kSceneEditorType})
                persistentTool<scene::SceneEditor>(entry);
            else if (entry.type.view() == lux::ui::PaneTypeIdView{material::kMaterialEditorType})
                persistentTool<material::MaterialEditor>(entry);
            else if (entry.type.view() == lux::ui::PaneTypeIdView{flowforge::kFlowForgeEditorType})
                persistentTool<flowforge::FlowForgeEditor>(entry);
            else if (entry.type.view() == lux::ui::PaneTypeIdView{"lux.editor.project"})
                persistentSingleton<ProjectPane>(entry);
            else if (entry.type.view() == lux::ui::PaneTypeIdView{"lux.editor.settings"})
                persistentSingleton<SettingPane>(entry);
            else if (entry.type.view() == lux::ui::PaneTypeIdView{"lux.editor.tasks"})
                persistentSingleton<ui::TaskPane>(entry);
        }
        std::vector<AssetEditorRegistration> assets(context.assetEditors().begin(), context.assetEditors().end());
        assets.push_back(
            {lux::ui::PaneTypeId{scene::kSceneEditorType},
             [](const ProjectAssetEntry& entry) noexcept { return entry.kind == EProjectAssetKind::SCENE; },
             &openTool<scene::SceneEditor>}
        );
        assets.push_back(
            {lux::ui::PaneTypeId{material::kMaterialEditorType},
             [](const ProjectAssetEntry& entry) noexcept { return entry.kind == EProjectAssetKind::MATERIAL_GRAPH; },
             &openTool<material::MaterialEditor>}
        );
        assets.push_back(
            {lux::ui::PaneTypeId{flowforge::kFlowForgeEditorType},
             [](const ProjectAssetEntry& entry) noexcept { return entry.kind == EProjectAssetKind::FLOW_GRAPH; },
             &openTool<flowforge::FlowForgeEditor>}
        );
        if (auto registered = context.panes().setRegistrations(std::move(panes)); !registered)
            return registered;
        if (auto registered = context.setAssetEditors(std::move(assets)); !registered)
            return registered;
        if (auto commands = assembleCommands(context); !commands)
            return commands;
        const auto project = context.panes().create(lux::ui::PaneTypeIdView{"lux.editor.project"});
        return project ? EditorResult<void>{} : lux::cxx::unexpected(project.error());
    }
}
