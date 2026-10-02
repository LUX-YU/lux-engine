#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <algorithm>

namespace lux::editor::application
{
    scene::SceneViewServices EditorApplication::Impl::sceneServices()
    {
        auto& rendering = *engine_->renderContext();
        return {
            sessions_.access<scene::SceneSession>(),
            projections_,
            engine_->sceneRuntime(),
            rendering.resources(),
            rendering.runtime(),
            environment_,
            runs_.inspect()
        };
    }
    EditorResult<views::ViewId> EditorApplication::Impl::adopt(views::DetachedView& candidate, std::string key)
    {
        auto result = desktop_->views().adopt(candidate, views::ViewRestoreKey{key});
        if (!result)
            return applicationFailure("view.adopt", result.error());
        return result->id;
    }
    EditorResult<sessions::OpenAssetId> EditorApplication::Impl::open(AssetReference reference)
    {
        if (phase_ != EApplicationPhase::RUNNING)
            return cxx::unexpected(EditorFailure{EEditorError::CLOSING, "application.open"});
        if (opens_.size() == 64)
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "application.open"});
        auto resolved = project_->resolveReference(reference, 0);
        if (!resolved)
            return cxx::unexpected(resolved.error());
        const auto* asset = project_->asset(*resolved);
        if (!asset)
            return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "project.source"});
        sessions::SessionKindId kind;
        switch (asset->kind)
        {
        case EProjectAssetKind::SCENE:
            kind = sessions::SessionKindId{"lux.editor.scene"};
            break;
        case EProjectAssetKind::MATERIAL_GRAPH:
            kind = sessions::SessionKindId{"lux.editor.material"};
            break;
        case EProjectAssetKind::FLOW_GRAPH:
            kind = sessions::SessionKindId{"lux.editor.flowforge"};
            break;
        default:
            return cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "asset.authoring"});
        }
        auto target = files_.resolve(asset->source_path);
        if (!target)
            return applicationFailure("source.target", target.error());
        auto source = project_->captureSource(asset->id, 64 * 1024 * 1024, target->expected_version);
        if (!source)
            return cxx::unexpected(source.error());
        sessions::OpenAssetRequest request{
            reference.project_instance,
            kind,
            {std::move(*source), asset->id, sessions::BoundSource{asset->id, target->key.value}, *target}
        };
        // A pinned catalog may outlive this short compound owner scope, never the scope itself.
        std::optional<sessions::OpenAssetId> id;
        auto open_source = [&](const auto& snapshot) -> extensions::ContributionResult<void> {
            auto opened = opening_.open(std::move(request), snapshot.sessions());
            if (!opened)
                return cxx::unexpected(extensions::ContributionFailure{
                    opened.error().code == sessions::ESessionFactoryError::BUSY
                        ? extensions::EContributionError::BUSY
                        : extensions::EContributionError::CALLBACK,
                    opened.error().domain,
                    opened.error().domain_code,
                    opened.error().detail
                });
            id = *opened;
            return {};
        };
        auto entered = contributions_.withSnapshot(open_source);
        if (!entered)
            return applicationFailure("open.admission", entered.error());
        opens_.push_back({*id});
        return *id;
    }
    EditorResult<OpenAndShowResult> EditorApplication::openStatus(sessions::OpenAssetId id) const
    {
        const auto entry = std::ranges::find(impl_->opens_, id, &Impl::OpenPresentation::operation);
        if (entry == impl_->opens_.end())
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "open.status"});
        auto status = impl_->opening_.status(id);
        if (!status)
            return applicationFailure("open.status", status.error());
        return OpenAndShowResult{std::move(*status), entry->view, entry->failure};
    }
    EditorResult<void> EditorApplication::cancelOpen(sessions::OpenAssetId id)
    {
        if (auto ready = impl_->admission(); !ready)
            return cxx::unexpected(ready.error());
        Impl::Dispatch scope{impl_->dispatching_};
        const auto entry = std::ranges::find(impl_->opens_, id, &Impl::OpenPresentation::operation);
        if (entry == impl_->opens_.end())
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "open.cancel"});
        auto cancelled = impl_->opening_.cancel(id);
        if (!cancelled)
            return applicationFailure("open.cancel", cancelled.error());
        entry->cancelled = true;
        return {};
    }
    EditorResult<void> EditorApplication::acknowledgeOpen(sessions::OpenAssetId id)
    {
        if (auto ready = impl_->admission(); !ready)
            return cxx::unexpected(ready.error());
        Impl::Dispatch scope{impl_->dispatching_};
        auto acknowledged = impl_->opening_.acknowledge(id);
        if (!acknowledged)
            return applicationFailure("open.acknowledge", acknowledged.error());
        std::erase_if(impl_->opens_, [id](const auto& entry) { return entry.operation == id; });
        return {};
    }
    EditorResult<void> EditorApplication::Impl::receiveOpenResults()
    {
        auto received = opening_.update();
        if (!received)
            return applicationFailure("open.receive", received.error());
        for (auto& entry : opens_)
        {
            if (entry.view || entry.failure || entry.cancelled)
                continue;
            const auto status = opening_.status(entry.operation);
            if (!status)
                return applicationFailure("open.status", status.error());
            if (status->stage != sessions::EOpenAssetStage::PUBLISHED)
                continue;
            auto shown = show(status->session, false);
            if (shown)
                entry.view = *shown;
            else
                entry.failure = std::move(shown.error()); // Content remains owned and queryable without a view.
        }
        return {};
    }
    EditorResult<views::ViewId> EditorApplication::Impl::show(sessions::SessionId id, bool another_view)
    {
        std::optional<EditorResult<views::ViewId>> result;
        auto prepare = [&](const extensions::ContributionSnapshot& snapshot) -> extensions::ContributionResult<void> {
            result.emplace(makeContentView(id, another_view, snapshot));
            return {};
        };
        auto guarded = contributions_.withSnapshot(prepare);
        if (!guarded)
            return applicationFailure("show.catalog", guarded.error());
        return std::move(*result);
    }
    EditorResult<views::ViewId> EditorApplication::Impl::makeContentView(
        sessions::SessionId id,
        bool another_view,
        const extensions::ContributionSnapshot& snapshot
    )
    {
        auto info = sessions_.describe(id);
        if (!info)
            return applicationFailure("show.session", info.error());
        if (!another_view)
            for (const auto& entry : content_views_)
                if (entry.session == id && desktop_->views().describe(entry.view))
                {
                    auto focused = desktop_->views().focus(entry.view);
                    if (!focused)
                        return applicationFailure("show.focus", focused.error());
                    return entry.view;
                }
        if (content_views_.size() == 64 || next_view_ == UINT64_MAX)
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "show.views"});
        const auto name = "content-" + std::to_string(next_view_++);
        ContentView owner{id};
        std::optional<views::ViewFactoryInput> input;
        views::ViewTypeId type;
        const auto source = info->binding ? info->binding->location : name;
        if (info->kind.name == "lux.editor.scene")
        {
            auto key = sessions_.key<scene::SceneSession>(id);
            if (!key)
                return applicationFailure("scene.key", key.error());
            owner.scene = std::make_unique<scene::SceneInteractionGroup>(
                sessions_.access<scene::SceneSession>(),
                *key,
                scene::InteractionGroupId{next_view_}
            );
            extensions::SceneViewInput value;
            value.binding = scene::EditedSceneBinding{*key, owner.scene.get()};
            value.title = source;
            value.state.camera.transform.translation = {0, 3, 8};
            // An explicit persisted viewport role is used when available; ambiguous scenes need a choice.
            auto scene = sessions_.access<scene::SceneSession>().read(*key);
            if (!scene)
                return applicationFailure("scene.read", scene.error());
            auto read_view = scene->get().read();
            if (!read_view)
                return applicationFailure("scene.read", read_view.error());
            auto chosen = read_view->withRead([&](const auto& read) -> scene::SceneEditResult<void> {
                const auto& description = read.configuration().scene->data();
                for (std::size_t i{}; i < description.systemCount(); ++i)
                    if (const auto system = description.systemAt(i);
                        system.type() == lux::scene::builtinRenderSystemRegistration().type)
                    {
                        if (value.render_system.value)
                            return cxx::unexpected(scene::SceneEditError{sessions::ESessionError::INVALID_ARGUMENT});
                        value.render_system = system.instanceId();
                    }
                return {};
            });
            if (!chosen)
                return applicationFailure("scene.viewport", chosen.error());
            type = views::ViewTypeId{"lux.editor.scene.view"};
            input.emplace(
                messages_.dispatcherRef(),
                lux::ui::PaneId{name},
                contracts::CodeLease::builtin(),
                cxx::typeToken<extensions::SceneViewInput>(),
                std::make_shared<const extensions::SceneViewInput>(std::move(value))
            );
        }
        else if (info->kind.name == "lux.editor.material")
        {
            auto key = sessions_.key<material::MaterialSession>(id);
            if (!key)
                return applicationFailure("material.key", key.error());
            owner.material =
                std::make_unique<material::MaterialInteraction>(sessions_.access<material::MaterialSession>(), *key);
            owner.preview = std::make_unique<material::MaterialPreviewStore>(
                engine_->sceneRuntime(),
                material::MaterialPreviewEnvironment{environment_, registrations_.features}
            );
            type = views::ViewTypeId{"lux.editor.material"};
            input.emplace(
                messages_.dispatcherRef(),
                lux::ui::PaneId{name},
                contracts::CodeLease::builtin(),
                cxx::typeToken<MaterialViewAssembly>(),
                std::make_shared<const MaterialViewAssembly>(
                    MaterialViewAssembly{material::MaterialViewBinding{*key, owner.material.get()}, owner.preview.get()}
                )
            );
        }
        else if (info->kind.name == "lux.editor.flowforge")
        {
            auto key = sessions_.key<flowforge::FlowSession>(id);
            if (!key)
                return applicationFailure("flow.key", key.error());
            owner.flow = std::make_unique<flowforge::FlowInteraction>(sessions_.access<flowforge::FlowSession>(), *key);
            type = views::ViewTypeId{"lux.editor.flowforge"};
            input.emplace(
                messages_.dispatcherRef(),
                lux::ui::PaneId{name},
                contracts::CodeLease::builtin(),
                cxx::typeToken<FlowViewAssembly>(),
                std::make_shared<const FlowViewAssembly>(
                    FlowViewAssembly{flowforge::FlowViewBinding{*key, owner.flow.get()}}
                )
            );
        }
        else
            return cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "show.kind"});
        auto view = snapshot.views().prepare(type, *input);
        if (!view)
            return applicationFailure("view.factory", view.error());
        if (owner.scene)
        {
            auto connected = object::LuxObject::connect(
                static_cast<scene::SceneView*>(view->pane()),
                &scene::SceneView::modelDropped,
                [this](scene::ModelPlacement placement) noexcept { receiveModel(std::move(placement)); }
            );
            if (!connected)
                return applicationFailure("scene.model.connect", connected.error());
            owner.model_drop = std::move(*connected);
        }
        if (owner.material)
        {
            auto connection = object::LuxObject::connect(
                static_cast<material::MaterialView*>(view->pane()),
                &material::MaterialView::publishRequested,
                [this](std::shared_ptr<const material::CompiledMaterial> compiled) noexcept {
                    receiveArtifact(std::move(compiled));
                }
            );
            if (!connection)
                return applicationFailure("material.publish.connect", connection.error());
            owner.publish = std::move(*connection);
        }
        if (owner.flow)
        {
            auto connection = object::LuxObject::connect(
                static_cast<flowforge::FlowView*>(view->pane()),
                &flowforge::FlowView::publishRequested,
                [this](std::shared_ptr<const flowforge::CompiledFlow> compiled) noexcept {
                    receiveArtifact(std::move(compiled));
                }
            );
            if (!connection)
                return applicationFailure("flow.publish.connect", connection.error());
            owner.publish = std::move(*connection);
        }
        auto adopted = adopt(*view, name);
        if (!adopted)
            return cxx::unexpected(adopted.error());
        owner.view = *adopted;
        const auto published = owner.view;
        content_views_.push_back(std::move(owner));
        return published;
    }
}
