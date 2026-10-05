#include <lux/engine/editor/scene/OutlinerView.hpp>
#include <lux/engine/editor/scene/RunInspectorView.hpp>
#include <lux/engine/editor/scene/SceneTools.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/workbench/UiFailure.hpp>
#include <lux/engine/ui/Root.hpp>

namespace lux::editor::scene
{
    cxx::expected<std::shared_ptr<SceneInteractionGroup>, lux::ui::EAttachmentError> shareSceneInteraction(
        lux::ui::Root& root,
        lux::ui::PaneHandle source
    )
    {
        std::shared_ptr<SceneInteractionGroup> result;
        const auto read = [&](lux::ui::Pane& pane)
        {
            if (auto* view = dynamic_cast<SceneView*>(&pane))
            {
                result = view->interactionOwner();
            }
            else if (auto* view = dynamic_cast<OutlinerView*>(&pane))
            {
                result = view->interactionOwner();
            }
            else if (auto* view = dynamic_cast<InspectorView*>(&pane))
            {
                result = view->interactionOwner();
            }
            else if (auto* view = dynamic_cast<RunInspectorView*>(&pane))
            {
                result = view->interactionOwner();
            }
        };
        auto inspected = root.withPane(source, read);
        if (!inspected)
        {
            return cxx::unexpected(inspected.error());
        }
        if (!result)
        {
            return cxx::unexpected(lux::ui::EAttachmentError::INVALID_TREE);
        }
        return result;
    }
    desktop::UiResult<std::unique_ptr<lux::ui::Pane, object::ObjectDeleter>> createSceneTool(
        desktop::UiRegistry& windows,
        services::ServiceRegistry& services,
        services::ServiceScope& parent,
        lux::ui::Root& root,
        lux::ui::PaneHandle source,
        ESceneTool kind,
        lux::ui::PaneId instance
    )
    {
        auto group = shareSceneInteraction(root, source);
        if (!group)
        {
            return cxx::unexpected(workbench::detail::uiFailure(
                group.error(), group.error() == lux::ui::EAttachmentError::BUSY, desktop::EUiError::ATTACHMENT
            ));
        }
        const desktop::UiDescriptor* descriptor{};
        switch (kind)
        {
        case ESceneTool::OUTLINER:
            descriptor = &kOutlinerView;
            break;
        case ESceneTool::INSPECTOR:
            descriptor = (*group)->run() ? &kRunInspectorView : &kInspectorView;
            break;
        case ESceneTool::RESOURCES:
            descriptor = &kResourceView;
            break;
        case ESceneTool::CONFIGURATION:
            descriptor = &kSceneConfigurationView;
            break;
        }
        if (!descriptor)
        {
            return cxx::unexpected(desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "scene.tool.kind"});
        }
        auto scope = services.createScope(&parent);
        if (!scope)
        {
            return cxx::unexpected(workbench::detail::uiFailure(
                scope.error(), workbench::detail::isRetryableUiFailure(scope.error()), desktop::EUiError::DEPENDENCY
            ));
        }
        const auto dependencies = [&]() -> services::ServiceResult<void>
        {
            if (auto provided = scope->provide(services::ServiceNameView{"lux.editor.scene.interaction"}, *group);
                !provided)
            {
                return provided;
            }
            if (auto provided = scope->provide(services::ServiceNameView{"lux.ui.root"}, root); !provided)
            {
                return provided;
            }
            return scope->provide(services::ServiceNameView{"lux.editor.scene.viewport"}, source);
        }();
        if (!dependencies)
        {
            return cxx::unexpected(workbench::detail::uiFailure(
                dependencies.error(),
                workbench::detail::isRetryableUiFailure(dependencies.error()),
                desktop::EUiError::DEPENDENCY
            ));
        }
        views::ViewContent content;
        if (const auto session = (*group)->session())
        {
            content = {{session->id()}, session->id()};
        }
        auto factory = windows.snapshot().find(descriptor->type);
        if (!factory)
        {
            return cxx::unexpected(std::move(factory.error()));
        }
        const auto restore_key = views::ViewRestoreKey{instance.name()};
        return windows.create(
            *factory,
            *scope,
            {root.dispatcherRef(), std::move(instance), std::move(content), {factory->descriptor().schema, {}}, restore_key}
        );
    }
} // namespace lux::editor::scene
