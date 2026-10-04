#include <lux/engine/editor/scene/InspectorView.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>

namespace lux::editor::scene
{
    struct InspectorView::Impl final
    {
        class Content final : public lux::ui::Element
        {
        public:
            explicit Content(lux::ui::Element& parent)
                : Element(parent, lux::ui::ElementId{"components"}), layout_(*this, lux::ui::ElementId{"fields"})
            {
                setVisible(false);
            }
            lux::ui::Layout& layout() noexcept
            {
                return layout_;
            }

        private:
            lux::ui::SizeHint sizeHintContent() noexcept override
            {
                return layout_.sizeHint();
            }
            lux::ui::SizeHint measureContent(float width) noexcept override
            {
                return layout_.measure(width);
            }
            void arrangeContent() noexcept override
            {
                layout_.arrange({{}, rect().size});
            }
            void draw() noexcept override
            {
                drawChild(layout_);
            }
            lux::ui::Layout layout_;
        };
        struct Component final
        {
            // The defining code outlives both the generated controls and their component cache.
            InspectorComponent registration;
            std::unique_ptr<InspectorFields> fields;
            std::unique_ptr<lux::ui::Element> controls;
            ~Component() noexcept
            {
                if (!clear())
                    std::terminate();
            }
            SceneEditResult<void> clear()
            {
                if (!fields)
                    return {};
                if (fields->active())
                {
                    auto cancelled = fields->cancel();
                    if (!cancelled)
                        return cancelled;
                }
                if (controls)
                {
                    auto read_action = [&]() -> SceneEditResult<void> {
                        controls.reset();
                        return {};
                    };
                    auto result = fields->withRead(read_action);
                    if (!result)
                    {
                        const auto& error = result.error();
                        const bool is_closed = error.code == ESceneEditError::SESSION &&
                                               error.session == sessions::ESessionError::STALE_SESSION;
                        if (!is_closed)
                            return result;
                        controls.reset();
                    }
                }
                return fields->release();
            }
        };
        std::shared_ptr<SceneInteractionGroup> selection_;
        sessions::TSessionAccess<SceneSession> sessions_;
        simulation::ecs::ComponentSchemaSet schemas_;
        std::vector<InspectorComponent> registrations_;
        project::ProjectCatalogModel* catalog_;
        std::optional<EditedSceneBinding> binding_;
        std::optional<SceneObjectRef> target_;
        std::vector<simulation::ecs::ComponentSchemaId> components_;
        std::vector<simulation::ecs::ComponentSchemaId> candidates_;
        std::vector<ApplicabilityResult> candidate_support_;
        std::optional<sessions::ContentStamp> candidates_at_;
        lux::ui::Layout layout_;
        lux::ui::Layout actions_;
        lux::ui::Choice component_choice_;
        lux::ui::Button add_, remove_, cancel_;
        lux::ui::Label message_;
        std::array<object::Connection, 3> connections_;
        bool cancel_requested_{};
        enum class EComponentAction : std::uint8_t
        {
            ADD,
            REMOVE
        };
        std::optional<EComponentAction> structure_request_;
        std::vector<std::unique_ptr<Component>> entries_;
        SceneEditResult<void> status_;
        Impl(
            InspectorView& view,
            sessions::TSessionAccess<SceneSession> sessions,
            simulation::ecs::ComponentSchemaSet schemas,
            std::vector<InspectorComponent> registrations,
            project::ProjectCatalogModel* catalog
        )
            : sessions_(sessions), schemas_(std::move(schemas)), registrations_(std::move(registrations)),
              catalog_(catalog), layout_(view, lux::ui::ElementId{"components"}),
              actions_(layout_, lux::ui::ElementId{"actions"}, lux::ui::ELayoutType::HORIZONTAL),
              component_choice_(actions_, lux::ui::ElementId{"schema"}, {}, -1),
              add_(actions_, lux::ui::ElementId{"add"}, "Add component"),
              remove_(actions_, lux::ui::ElementId{"remove"}, "Remove component"),
              cancel_(actions_, lux::ui::ElementId{"cancel"}, "Cancel field draft"),
              message_(layout_, lux::ui::ElementId{"message"}, "")
        {
            view.setContent(layout_);
            auto add = object::LuxObject::connect(&add_, &lux::ui::Button::activated, [this]() noexcept {
                if (!structure_request_)
                    structure_request_ = EComponentAction::ADD;
            });
            auto remove = object::LuxObject::connect(&remove_, &lux::ui::Button::activated, [this]() noexcept {
                if (!structure_request_)
                    structure_request_ = EComponentAction::REMOVE;
            });
            auto cancel = object::LuxObject::connect(&cancel_, &lux::ui::Button::activated, [this]() noexcept {
                cancel_requested_ = true;
            });
            if (!add || !remove || !cancel)
                status_ = InspectorFields::constructionFailure();
            else
            {
                connections_[0] = std::move(*add);
                connections_[1] = std::move(*remove);
                connections_[2] = std::move(*cancel);
            }
        }
        SceneEditResult<void> updateCandidates(const SceneReadView& read)
        {
            return read.withRead([&](const SceneReadView& source) -> SceneEditResult<void> {
                if (!target_ || !source.contains(*target_))
                    return cxx::unexpected(SceneEditError{ESceneEditError::STALE_OBJECT});
                const auto facts = source.facts();
                if (candidates_at_ == facts.based_on)
                    return {};
                const auto previous = component_choice_.value();
                const bool has_previous = previous >= 0 && static_cast<std::size_t>(previous) < candidates_.size();
                const auto selected = has_previous ? std::optional{candidates_[previous]} : std::nullopt;
                std::vector<simulation::ecs::ComponentSchemaId> ids;
                std::vector<ApplicabilityResult> support;
                for (const auto& schema : facts.schemas)
                {
                    const auto id = simulation::ecs::componentSchemaId(schema.name);
                    const std::string_view names[]{schema.name};
                    auto allowed = queryApplicability(facts, {names, true, true});
                    const bool present = std::ranges::find(components_, id) != components_.end();
                    if (!allowed.supported() && !present)
                        continue;
                    ids.push_back(id);
                    support.push_back(std::move(allowed));
                }
                std::vector<lux::ui::ChoiceOption> options;
                for (std::size_t i{}; i < ids.size(); ++i)
                    options.push_back({static_cast<std::int64_t>(i), ids[i].name});
                component_choice_.setValue(-1);
                component_choice_.setOptions(std::move(options));
                if (selected)
                {
                    const auto found = std::ranges::find(ids, *selected);
                    if (found != ids.end())
                        component_choice_.setValue(found - ids.begin());
                }
                candidates_ = std::move(ids);
                candidate_support_ = std::move(support);
                candidates_at_ = facts.based_on;
                return {};
            });
        }
        SceneEditResult<void> changeComponent(const simulation::ecs::ComponentSchemaId& id, bool add)
        {
            if (!binding_ || !target_)
                return cxx::unexpected(SceneEditError{ESceneEditError::STALE_OBJECT});
            auto finished = finish();
            if (!finished)
                return finished;
            auto session = sessions_.read(binding_->session);
            if (!session)
                return cxx::unexpected(SceneEditError{session.error()});
            auto read = session->get().read();
            if (!read)
                return cxx::unexpected(read.error());
            SceneEditBatch batch{session->get().describe().current, add ? "Add component" : "Remove component", {}};
            if (add)
            {
                const auto* schema = schemas_.find(id);
                if (!schema || !schema->create)
                    return cxx::unexpected(SceneEditError{ESceneEditError::MISSING_SCHEMA});
                auto encoded = read->withRead([&](const SceneReadView&) -> SceneEditResult<SceneComponentData> {
                    const std::string_view names[]{id.name};
                    auto allowed = queryApplicability(read->facts(), {names, true, true});
                    if (!allowed.supported())
                        return cxx::unexpected(SceneEditError{ESceneEditError::INVALID_COMPONENT});
                    try
                    {
                        auto value = schema->create(schema->code_lifetime);
                        if (!value)
                            return cxx::unexpected(SceneEditError{ESceneEditError::CODEC});
                        simulation::ecs::WorldEntityMap identities;
                        auto bytes = value->encode(identities, 16 * 1024 * 1024);
                        if (!bytes)
                            return cxx::unexpected(SceneEditError{ESceneEditError::CODEC});
                        return SceneComponentData{schema->id, schema->version, std::move(*bytes)};
                    }
                    catch (const std::bad_alloc&)
                    {
                        std::terminate();
                    }
                    catch (...)
                    {
                        return cxx::unexpected(SceneEditError{ESceneEditError::CODEC});
                    }
                });
                if (!encoded)
                    return cxx::unexpected(encoded.error());
                batch.edits.emplace_back(SceneAddComponent{*target_, std::move(*encoded)});
            }
            else
                batch.edits.emplace_back(SceneRemoveComponent{{*target_, id, {}}});
            auto owner = sessions_.edit(binding_->session);
            if (!owner)
                return cxx::unexpected(SceneEditError{owner.error()});
            auto result = owner->get().apply(std::move(batch));
            if (!result)
                return cxx::unexpected(result.error());
            return {};
        }

        SceneEditResult<void> clear()
        {
            // Admission is checked before touching any control subtree. BUSY preserves the view.
            if (binding_)
            {
                auto session = sessions_.read(binding_->session);
                if (!session && session.error() != sessions::ESessionError::STALE_SESSION)
                    return cxx::unexpected(SceneEditError{session.error()});
                if (session)
                {
                    auto read = session->get().read();
                    if (!read)
                        return cxx::unexpected(read.error());
                    auto admitted = read->withRead([](const SceneReadView&) -> SceneEditResult<void> { return {}; });
                    if (!admitted)
                        return admitted;
                }
            }
            for (auto& entry : entries_)
                if (auto result = entry->clear(); !result)
                    return result;
            entries_.clear();
            components_.clear();
            candidates_at_.reset();
            target_.reset();
            binding_.reset();
            return {};
        }
        SceneEditResult<void> rebind(EditedSceneBinding binding, SceneObjectRef target)
        {
            if (object::LuxObject::isDispatching())
                return cxx::unexpected(SceneEditError{ESceneEditError::BUSY});
            const bool is_owner_mismatch = selection_ && selection_.get() != binding.interaction;
            const bool invalid = is_owner_mismatch || !binding.interaction ||
                                 binding.interaction->session() != binding.session ||
                                 target.session != binding.session.id();
            if (invalid)
                return cxx::unexpected(SceneEditError{ESceneEditError::STALE_OBJECT});
            auto session = sessions_.read(binding.session);
            if (!session)
                return cxx::unexpected(SceneEditError{session.error()});
            auto read = session->get().read();
            if (!read)
                return cxx::unexpected(read.error());
            auto components = read->components(target);
            if (!components)
                return cxx::unexpected(components.error());
            // A hidden candidate participates only in owner maintenance. The old displayed subtree
            // remains until all factories and interaction completion succeed. Initial construction
            // still occurs inside the factory's entirely detached Pane.
            auto candidate_root = std::make_unique<Content>(layout_);
            std::vector<std::unique_ptr<Component>> candidate;
            for (const auto& id : *components)
            {
                const auto* schema = schemas_.find(id);
                if (!schema)
                    continue; // Unknown encoded values remain owned by the author model.
                const auto found = std::ranges::find(registrations_, schema->cpp_type, &InspectorComponent::type);
                if (found == registrations_.end() || !found->create)
                    continue;
                auto entry = std::make_unique<Component>();
                entry->registration = *found;
                entry->fields =
                    std::make_unique<InspectorFields>(sessions_, *binding.interaction, target, *schema, catalog_);
                auto refreshed = entry->fields->refresh();
                if (!refreshed)
                    return refreshed;
                auto read_action = [&]() -> SceneEditResult<void> {
                    auto element = found->create(candidate_root->layout(), lux::ui::ElementId{id.name}, *entry->fields);
                    if (!element)
                        return cxx::unexpected(element.error());
                    entry->controls = std::move(*element);
                    return {};
                };
                auto created = entry->fields->withRead(read_action);
                if (!created)
                    return created;
                candidate.push_back(std::move(entry));
            }
            auto cleared = clear();
            if (!cleared)
                return cleared;
            candidate_root->setVisible(true);
            root_ = std::move(candidate_root);
            entries_ = std::move(candidate);
            binding_ = binding;
            target_ = target;
            components_ = std::move(*components);
            return updateCandidates(*read);
        }
        SceneEditResult<void> finish()
        {
            for (auto& entry : entries_)
                if (!entry->fields->finish())
                    return entry->fields->status();
            return {};
        }
        SceneEditResult<void> cancel()
        {
            for (auto& entry : entries_)
                if (auto result = entry->fields->cancel(); !result)
                    return result;
            structure_request_.reset();
            return {};
        }
        void update()
        {
            if (!binding_)
                return;
            if (cancel_requested_)
            {
                status_ = cancel();
                if (!status_)
                    return;
                cancel_requested_ = false;
            }
            if (structure_request_)
            {
                const auto index = component_choice_.value();
                if (index >= 0 && static_cast<std::size_t>(index) < candidates_.size())
                    status_ = changeComponent(candidates_[index], *structure_request_ == EComponentAction::ADD);
                else
                    status_ = cxx::unexpected(SceneEditError{ESceneEditError::MISSING_SCHEMA});
                if (!status_)
                {
                    message_.setText("Component operation rejected; previous fields retained.");
                    const auto& error = status_.error();
                    const bool busy =
                        error.code == ESceneEditError::BUSY ||
                        (error.code == ESceneEditError::SESSION && error.session == sessions::ESessionError::BUSY);
                    if (!busy)
                        structure_request_.reset();
                    return;
                }
                structure_request_.reset();
            }
            auto session = sessions_.read(binding_->session);
            if (!session)
            {
                status_ = cxx::unexpected(SceneEditError{session.error()});
                return;
            }
            auto read = session->get().read();
            if (!read)
            {
                status_ = cxx::unexpected(read.error());
                return;
            }
            auto components = read->components(*target_);
            if (!components)
            {
                const auto failure = components.error();
                if (failure.code == ESceneEditError::STALE_OBJECT || failure.code == ESceneEditError::INVALID_OBJECT)
                {
                    auto cleared = clear();
                    if (!cleared)
                    {
                        status_ = cleared;
                        return;
                    }
                    root_.reset();
                }
                status_ = cxx::unexpected(failure);
                return;
            }
            if (*components != components_)
            {
                status_ = rebind(*binding_, *target_);
                return;
            }
            for (auto& entry : entries_)
            {
                status_ = entry->fields->update();
                if (!status_)
                {
                    message_.setText(
                        "Field operation rejected; cancel the draft to recover. Author content is retained."
                    );
                    return;
                }
            }
            status_ = updateCandidates(*read);
            if (!status_)
                return;
            const auto index = component_choice_.value();
            const bool valid = index >= 0 && static_cast<std::size_t>(index) < candidates_.size();
            const bool present = valid && std::ranges::find(components_, candidates_[index]) != components_.end();
            add_.setEnabled(valid && !present && candidate_support_[index].supported());
            remove_.setEnabled(present);
            const bool retained_unknown = present && candidate_support_[index].reason == EApplicabilityReason::MISSING_PROVIDER;
            message_.setText(retained_unknown ? "Component provider unavailable; encoded content is preserved." : "");
        }
        // Declared before destruction is needed by entries; explicit clear runs while it is alive.
        std::unique_ptr<Content> root_;
        ~Impl() noexcept
        {
            if (!clear())
                std::terminate();
        }
    };
    InspectorView::InspectorView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        sessions::TSessionAccess<SceneSession> sessions,
        simulation::ecs::ComponentSchemaSet schemas,
        std::vector<InspectorComponent> registrations,
        project::ProjectCatalogModel* catalog,
        std::shared_ptr<SceneInteractionGroup> selection
    )
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.inspector"}, "Inspector"),
          impl_(std::make_unique<Impl>(*this, sessions, std::move(schemas), std::move(registrations), catalog))
    {
        impl_->selection_ = std::move(selection);
    }
    const std::shared_ptr<SceneInteractionGroup>& InspectorView::interactionOwner() const noexcept
    {
        return impl_->selection_;
    }
    InspectorView::~InspectorView() noexcept = default;
    SceneEditResult<void> InspectorView::rebind(EditedSceneBinding binding, SceneObjectRef target)
    {
        return impl_->rebind(binding, target);
    }
    SceneEditResult<void> InspectorView::clearTarget()
    {
        if (object::LuxObject::isDispatching())
            return cxx::unexpected(SceneEditError{ESceneEditError::BUSY});
        auto cleared = impl_->clear();
        if (!cleared)
            return cleared;
        impl_->root_.reset();
        impl_->structure_request_.reset();
        impl_->add_.setEnabled(false);
        impl_->remove_.setEnabled(false);
        impl_->status_ = {};
        return {};
    }
    SceneEditResult<void> InspectorView::finishEditing()
    {
        return impl_->finish();
    }
    SceneEditResult<void> InspectorView::cancelEditing()
    {
        return impl_->cancel();
    }
    SceneEditResult<void> InspectorView::addComponent(const simulation::ecs::ComponentSchemaId& schema)
    {
        return impl_->changeComponent(schema, true);
    }
    SceneEditResult<void> InspectorView::removeComponent(const simulation::ecs::ComponentSchemaId& schema)
    {
        return impl_->changeComponent(schema, false);
    }
    SceneEditResult<void> InspectorView::prepareClose()
    {
        return impl_->cancel();
    }
    views::ViewContent InspectorView::content() const noexcept
    {
        if (impl_->selection_ && impl_->selection_->session())
        {
            const auto id = impl_->selection_->session()->id();
            return {{id}, id};
        }
        if (impl_->binding_)
        {
            const auto id = impl_->binding_->session.id();
            return {{id}, id};
        }
        return {};
    }
    std::optional<SceneObjectRef> InspectorView::target() const noexcept
    {
        return impl_->target_;
    }
    const SceneEditResult<void>& InspectorView::status() const noexcept
    {
        return impl_->status_;
    }
    void InspectorView::update() noexcept
    {
        if (impl_->selection_)
        {
            auto synchronized = impl_->selection_->synchronize();
            if (!synchronized)
            {
                impl_->status_ = std::move(synchronized);
                return;
            }
            const auto& selection = impl_->selection_->selection().objects;
            const auto* target = selection.empty() ? nullptr : std::get_if<SceneObjectRef>(&selection.front());
            const auto session = impl_->selection_->session();
            SceneEditResult<void> changed;
            if (target && session && impl_->target_ != *target)
                changed = rebind({*session, impl_->selection_.get()}, *target);
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
    SceneViewResult<views::DetachedView> makeInspectorView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        sessions::TSessionAccess<SceneSession> sessions,
        EditedSceneBinding binding,
        SceneObjectRef target,
        simulation::ecs::ComponentSchemaSet schemas,
        std::vector<InspectorComponent> registrations,
        project::ProjectCatalogModel* catalog,
        std::shared_ptr<SceneInteractionGroup> selection
    )
    {
        auto view = std::make_unique<InspectorView>(
            dispatcher,
            std::move(id),
            sessions,
            std::move(schemas),
            std::move(registrations),
            catalog,
            std::move(selection)
        );
        if (!view->status())
            return cxx::unexpected(SceneViewFailure{view->status().error()});
        auto bound = view->rebind(binding, target);
        if (!bound)
            return cxx::unexpected(SceneViewFailure{bound.error()});
        return views::DetachedView{
            lux::object::CodeLease::builtin(),
            std::move(view),
            +[](lux::ui::Pane& pane) -> views::ViewCloseResult {
                auto cleared = static_cast<InspectorView&>(pane).prepareClose();
                if (!cleared)
                    return cxx::unexpected(views::ViewPreparationFailure{
                        cleared.error().code == ESceneEditError::SESSION ? "session" : "scene.edit",
                        cleared.error().code == ESceneEditError::SESSION
                            ? static_cast<std::uint64_t>(cleared.error().session)
                            : static_cast<std::uint64_t>(cleared.error().code),
                        "Field interaction could not be ended",
                        cleared.error().code == ESceneEditError::BUSY ||
                            (cleared.error().code == ESceneEditError::SESSION &&
                             cleared.error().session == sessions::ESessionError::BUSY)
                    });
                return {};
            },
            nullptr, nullptr, nullptr,
            +[](const lux::ui::Pane& pane) noexcept { return static_cast<const InspectorView&>(pane).content(); }
        };
    }
}
