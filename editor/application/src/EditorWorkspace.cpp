#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <algorithm>

namespace lux::editor::application
{
    EditorResult<void> EditorApplication::applyLayout(workspace::DockLayout layout)
    {
        if (auto ready = impl_->admission(); !ready)
            return ready;
        Impl::Dispatch scope{impl_->dispatching_};
        return impl_->applyLayout(std::move(layout));
    }
    EditorResult<void> EditorApplication::Impl::applyLayout(workspace::DockLayout layout)
    {
        if (phase_ != EApplicationPhase::RUNNING)
            return cxx::unexpected(EditorFailure{EEditorError::CLOSING, "layout.application"});
        std::vector<ContentView> owners;
        std::vector<std::string> pane_names;
        EditorResult<void> result;
        auto create_input = [&](views::ViewTypeId type,
                                lux::ui::PaneId id) -> views::ViewFactoryResult<views::ViewFactoryInput> {
            if (content_views_.size() + owners.size() >= 64)
                return cxx::unexpected(views::ViewFactoryFailure{views::EViewFactoryError::CONSTRUCT, "layout.capacity"}
                );
            const auto make_input = [&](auto value) {
                using Value = decltype(value);
                return views::ViewFactoryInput{
                    messages_.dispatcherRef(),
                    id,
                    contracts::CodeLease::builtin(),
                    cxx::typeToken<Value>(),
                    std::make_shared<const Value>(std::move(value))
                };
            };
            if (type == views::ViewTypeId{"lux.editor.scene.view"})
                return make_input(extensions::SceneViewInput{});
            if (type == views::ViewTypeId{"lux.editor.material"})
            {
                ContentView owner;
                owner.preview = std::make_unique<material::MaterialPreviewStore>(
                    engine_->sceneRuntime(),
                    material::MaterialPreviewEnvironment{environment_, registrations_.features}
                );
                auto input = make_input(MaterialViewAssembly{{}, owner.preview.get(), {}});
                pane_names.emplace_back(id.name());
                owners.push_back(std::move(owner));
                return input;
            }
            if (type == views::ViewTypeId{"lux.editor.flowforge"})
                return make_input(FlowViewAssembly{});
            return make_input(EmptyViewInput{}); // Exact factory argument validation rejects unsupported types.
        };
        auto apply = [&](const extensions::ContributionSnapshot& snapshot) -> extensions::ContributionResult<void> {
            auto prepared = desktop_->views().prepareLayout(std::move(layout), snapshot.views(), create_input);
            if (!prepared)
            {
                result = applicationFailure("layout.prepare", prepared.error());
                return {};
            }
            auto committed = desktop_->views().commit(*prepared);
            if (!committed)
            {
                result = applicationFailure("layout.commit", committed.error());
                return {};
            }
            // Commit is already a fact. Every adopted preview now has its stable application owner.
            const auto all = desktop_->views().describeAll();
            if (!all)
                std::terminate();
            for (std::size_t i{}; i < owners.size(); ++i)
            {
                for (const auto& view : *all)
                {
                    bool matched{};
                    auto compare = [&](lux::ui::Pane& pane) { matched = pane.id().name() == pane_names[i]; };
                    auto visited = desktop_->views().withView(view.id, compare);
                    if (!visited)
                        std::terminate();
                    if (matched)
                    {
                        owners[i].view = view.id;
                        break;
                    }
                }
                if (!owners[i].view.valid())
                    std::terminate();
                content_views_.push_back(std::move(owners[i]));
            }
            return {};
        };
        auto entered = contributions_.withSnapshot(apply);
        if (!entered)
            return applicationFailure("layout.catalog", entered.error());
        return result;
    }
}
