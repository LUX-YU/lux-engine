#include <lux/engine/editor/scene/RunInspectorView.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/editor/scene/SceneInteraction.hpp>

namespace lux::editor::scene
{
    namespace
    {
        bool ended(const RunFailure& failure) noexcept
        {
            const auto* error = std::get_if<ERunError>(&failure.cause);
            return error && (*error == ERunError::INVALID_ID || *error == ERunError::STOPPED);
        }
        views::ViewPreparationFailure preparationFailure(const RunFailure& failure)
        {
            if (const auto* error = std::get_if<ERunError>(&failure.cause))
                return {
                    "run",
                    static_cast<std::uint64_t>(*error),
                    "Run inspection unavailable",
                    *error == ERunError::BUSY || *error == ERunError::NOT_READY
                };
            if (const auto* error = std::get_if<editing::EditFailure>(&failure.cause))
                return {
                    "run.edit",
                    static_cast<std::uint64_t>(error->code),
                    "Run field editing unavailable",
                    error->code == editing::EEditError::BUSY
                };
            if (const auto* runtime = std::get_if<lux::scene::SceneRuntimeFailure>(&failure.cause))
                if (const auto* error = std::get_if<lux::scene::ESceneRuntimeError>(&runtime->cause))
                    return {
                        "scene.runtime",
                        static_cast<std::uint64_t>(*error),
                        "Run instance unavailable",
                        *error == lux::scene::ESceneRuntimeError::BUSY
                    };
            return {"run.inspector", failure.cause.index(), "Run inspection rejected", false};
        }
    }

    struct RunInspectorView::Impl final
    {
        struct Component final
        {
            // Defining code outlives the controls' and copied component's destructor tails.
            RunInspectorComponent registration;
            std::unique_ptr<RunInspectorFields> fields;
            std::unique_ptr<lux::ui::Element> controls;
            RunResult<void> clear()
            {
                if (!fields)
                    return {};
                auto cancelled = fields->cancel();
                if (!cancelled)
                    return cancelled;
                if (controls)
                {
                    const auto destroy = [&]() -> RunResult<void> {
                        controls.reset();
                        return {};
                    };
                    auto result = fields->withRead(destroy);
                    if (!result)
                    {
                        if (!ended(result.error()))
                            return result;
                        controls.reset();
                    }
                }
                return fields->release();
            }
            ~Component() noexcept
            {
                if (!clear())
                    std::terminate();
            }
        };
        std::shared_ptr<SceneInteractionGroup> selection_;
        RunStore& runs_;
        simulation::ecs::ComponentSchemaSet schemas_;
        std::vector<RunInspectorComponent> registrations_;
        project::ProjectCatalogModel* catalog_;
        lux::ui::Layout layout_;
        lux::ui::Label message_;
        lux::ui::Button cancel_;
        object::Connection cancelled_;
        std::optional<RunningObjectRef> target_;
        std::unique_ptr<lux::ui::Layout> fields_;
        std::vector<std::unique_ptr<Component>> components_;
        std::vector<cxx::TypeToken> types_;
        bool cancel_requested_{};
        RunResult<void> status_;
        Impl(
            RunInspectorView& pane,
            RunStore& runs,
            simulation::ecs::ComponentSchemaSet schemas,
            std::vector<RunInspectorComponent> registrations,
            project::ProjectCatalogModel* catalog
        )
            : runs_(runs), schemas_(std::move(schemas)), registrations_(std::move(registrations)), catalog_(catalog),
              layout_(pane, lux::ui::ElementId{"run-inspector"}), message_(layout_, lux::ui::ElementId{"status"}, ""),
              cancel_(layout_, lux::ui::ElementId{"cancel"}, "Cancel field draft")
        {
            pane.setContent(layout_);
            auto connection = object::LuxObject::connect(&cancel_, &lux::ui::Button::activated, [this]() noexcept {
                cancel_requested_ = true;
            });
            if (!connection)
                status_ = RunInspectorFields::connectionFailure(connection.error());
            else
                cancelled_ = std::move(*connection);
        }
        RunResult<void> cancel()
        {
            for (auto& component : components_)
                if (auto result = component->fields->cancel(); !result)
                    return result;
            return {};
        }
        RunResult<void> clear()
        {
            for (auto& component : components_)
                if (auto result = component->clear(); !result)
                    return result;
            components_.clear();
            fields_.reset();
            target_.reset();
            types_.clear();
            return {};
        }
        RunResult<void> rebind(RunningObjectRef target)
        {
            if (object::LuxObject::isDispatching())
                return cxx::unexpected(RunFailure{ERunError::BUSY});
            if (selection_ && selection_->run() != target.run)
                return cxx::unexpected(RunFailure{ERunError::INVALID_ID});
            auto candidate = std::make_unique<lux::ui::Layout>(layout_, lux::ui::ElementId{"fields"});
            candidate->setVisible(false);
            std::vector<std::unique_ptr<Component>> entries;
            std::vector<cxx::TypeToken> types;
            const auto enumerate = [&](const simulation::ecs::Registry& registry,
                                       const std::optional<editing::HistorySnapshot>&) -> RunResult<void> {
                for (const auto& schema : schemas_.all())
                    if (schema.operations.has(registry, target.entity))
                        types.push_back(schema.cpp_type);
                return {};
            };
            auto checked = runs_.withInspection(target, enumerate);
            if (!checked)
                return checked;
            for (auto type : types)
            {
                const auto registered = std::ranges::find(registrations_, type, &RunInspectorComponent::type);
                if (registered == registrations_.end() || !registered->create || !registered->copy)
                    continue;
                const auto* schema = schemas_.find(type);
                auto entry = std::make_unique<Component>();
                entry->registration = *registered;
                entry->fields =
                    std::make_unique<RunInspectorFields>(runs_, target, *schema, registered->copy, catalog_);
                auto refreshed = entry->fields->refresh();
                if (!refreshed)
                    return refreshed;
                const auto create = [&]() -> RunResult<void> {
                    auto controls = registered->create(*candidate, lux::ui::ElementId{schema->id.name}, *entry->fields);
                    if (!controls)
                        return cxx::unexpected(controls.error());
                    entry->controls = std::move(*controls);
                    return {};
                };
                auto built = entry->fields->withRead(create);
                if (!built)
                    return built;
                entries.push_back(std::move(entry));
            }
            auto cleared = clear();
            if (!cleared)
                return cleared;
            candidate->setVisible(true);
            fields_ = std::move(candidate);
            components_ = std::move(entries);
            types_ = std::move(types);
            target_ = target;
            return {};
        }
        void update()
        {
            if (!target_)
                return;
            if (cancel_requested_)
            {
                status_ = cancel();
                if (!status_)
                    return;
                cancel_requested_ = false;
            }
            std::vector<cxx::TypeToken> types;
            const auto enumerate = [&](const simulation::ecs::Registry& registry,
                                       const std::optional<editing::HistorySnapshot>&) -> RunResult<void> {
                for (const auto& schema : schemas_.all())
                    if (schema.operations.has(registry, target_->entity))
                        types.push_back(schema.cpp_type);
                return {};
            };
            status_ = runs_.withInspection(*target_, enumerate);
            if (status_ && types != types_)
                status_ = rebind(*target_);
            if (status_)
                for (auto& component : components_)
                {
                    status_ = component->fields->update();
                    if (!status_)
                        break;
                }
            message_.setText(
                status_ ? "Running values are read-only; pause to edit."
                        : "Run changed or unavailable. Retry or cancel the draft."
            );
        }
        ~Impl() noexcept
        {
            if (!clear())
                std::terminate();
        }
    };
    RunInspectorView::RunInspectorView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        RunStore& runs,
        simulation::ecs::ComponentSchemaSet schemas,
        std::vector<RunInspectorComponent> registrations,
        project::ProjectCatalogModel* catalog,
        std::shared_ptr<SceneInteractionGroup> selection
    )
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.run-inspector"}, "Run Inspector"),
          impl_(std::make_unique<Impl>(*this, runs, std::move(schemas), std::move(registrations), catalog))
    {
        impl_->selection_ = std::move(selection);
    }
    const std::shared_ptr<SceneInteractionGroup>& RunInspectorView::interactionOwner() const noexcept
    {
        return impl_->selection_;
    }
    RunInspectorView::~RunInspectorView() noexcept = default;
    RunResult<void> RunInspectorView::rebind(RunningObjectRef target)
    {
        return impl_->rebind(target);
    }
    RunResult<void> RunInspectorView::clearTarget()
    {
        if (object::LuxObject::isDispatching())
            return cxx::unexpected(RunFailure{ERunError::BUSY});
        auto cleared = impl_->clear();
        if (!cleared)
            return cleared;
        impl_->cancel_requested_ = false;
        impl_->status_ = {};
        return {};
    }
    RunResult<void> RunInspectorView::finishEditing()
    {
        for (auto& entry : impl_->components_)
            if (!entry->fields->finish())
                return entry->fields->status();
        return {};
    }
    RunResult<void> RunInspectorView::cancelEditing()
    {
        return impl_->cancel();
    }
    RunResult<void> RunInspectorView::prepareClose()
    {
        // Preparation may be abandoned when another view refuses the same batch.
        // End the draft now; keep the binding/controls until ownership actually retires.
        return impl_->cancel();
    }
    std::optional<RunningObjectRef> RunInspectorView::target() const noexcept
    {
        return impl_->target_;
    }
    const RunResult<void>& RunInspectorView::status() const noexcept
    {
        return impl_->status_;
    }
    void RunInspectorView::update() noexcept
    {
        if (impl_->selection_)
        {
            auto synchronized = impl_->selection_->synchronize();
            if (!synchronized)
            {
                impl_->status_ = cxx::unexpected(RunFailure{synchronized.error()});
                return;
            }
            const auto& selection = impl_->selection_->selection().objects;
            const auto* target = selection.empty() ? nullptr : std::get_if<RunningObjectRef>(&selection.front());
            RunResult<void> changed;
            if (target && impl_->target_ != *target)
                changed = rebind(*target);
            else if (!target && impl_->target_)
                changed = clearTarget();
            if (!changed)
            {
                impl_->status_ = std::move(changed);
                return;
            }
        }
        impl_->update();
    }
    RunResult<views::DetachedView> makeRunInspectorView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        RunStore& runs,
        RunningObjectRef target,
        simulation::ecs::ComponentSchemaSet schemas,
        std::vector<RunInspectorComponent> registrations,
        project::ProjectCatalogModel* catalog,
        std::shared_ptr<SceneInteractionGroup> selection
    )
    {
        auto view = std::make_unique<RunInspectorView>(
            dispatcher,
            std::move(id),
            runs,
            std::move(schemas),
            std::move(registrations),
            catalog,
            std::move(selection)
        );
        if (!view->status())
            return cxx::unexpected(view->status().error());
        auto bound = view->rebind(target);
        if (!bound)
            return cxx::unexpected(bound.error());
        const auto close = +[](lux::ui::Pane& pane) -> views::ViewCloseResult {
            auto result = static_cast<RunInspectorView&>(pane).prepareClose();
            if (result)
                return {};
            return cxx::unexpected(preparationFailure(result.error()));
        };
        return views::DetachedView{
            lux::object::CodeLease::builtin(),
            std::move(view),
            close,
            +[](lux::ui::Pane& pane) -> views::ViewCloseResult {
                auto result = static_cast<RunInspectorView&>(pane).cancelEditing();
                if (result)
                    return {};
                return cxx::unexpected(preparationFailure(result.error()));
            }
        };
    }
}
