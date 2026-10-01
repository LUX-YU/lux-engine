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
        sessions::TSessionAccess<SceneSession> sessions_;
        simulation::ecs::ComponentSchemaSet schemas_;
        std::vector<InspectorComponent> registrations_;
        project::ProjectCatalogModel* catalog_;
        std::optional<EditedSceneBinding> binding_;
        std::optional<SceneObjectRef> target_;
        std::vector<simulation::ecs::ComponentSchemaId> components_;
        lux::ui::Layout layout_;
        lux::ui::Layout actions_;
        lux::ui::Choice component_choice_;
        lux::ui::Button add_, remove_;
        lux::ui::Label message_;
        std::array<object::Connection, 2> connections_;
        std::optional<bool> structure_request_;
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
              component_choice_(actions_, lux::ui::ElementId{"schema"}, options(schemas_), 0),
              add_(actions_, lux::ui::ElementId{"add"}, "Add component"),
              remove_(actions_, lux::ui::ElementId{"remove"}, "Remove component"),
              message_(layout_, lux::ui::ElementId{"message"}, "")
        {
            view.setContent(layout_);
            auto add = object::LuxObject::connect(&add_, &lux::ui::Button::activated, [this]() noexcept {
                if (!structure_request_)
                    structure_request_ = true;
            });
            auto remove = object::LuxObject::connect(&remove_, &lux::ui::Button::activated, [this]() noexcept {
                if (!structure_request_)
                    structure_request_ = false;
            });
            if (!add || !remove)
                status_ = InspectorFields::constructionFailure();
            else
            {
                connections_[0] = std::move(*add);
                connections_[1] = std::move(*remove);
            }
        }
        static std::vector<lux::ui::ChoiceOption> options(const simulation::ecs::ComponentSchemaSet& schemas)
        {
            std::vector<lux::ui::ChoiceOption> result;
            for (const auto& schema : schemas.all())
                result.push_back({static_cast<std::int64_t>(result.size()), schema.id.name});
            return result;
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
            target_.reset();
            binding_.reset();
            return {};
        }
        SceneEditResult<void> rebind(EditedSceneBinding binding, SceneObjectRef target)
        {
            if (object::LuxObject::isDispatching())
                return cxx::unexpected(SceneEditError{ESceneEditError::BUSY});
            const bool invalid = !binding.interaction || binding.interaction->session() != binding.session ||
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
            return {};
        }
        SceneEditResult<void> finish()
        {
            for (auto& entry : entries_)
                if (!entry->fields->finish())
                    return entry->fields->status();
            return {};
        }
        void update()
        {
            if (!binding_)
                return;
            if (structure_request_)
            {
                const auto index = component_choice_.value();
                if (index >= 0 && static_cast<std::size_t>(index) < schemas_.all().size())
                    status_ = changeComponent(schemas_.all()[index].id, *structure_request_);
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
                    return;
            }
            const auto index = component_choice_.value();
            const bool valid = index >= 0 && static_cast<std::size_t>(index) < schemas_.all().size();
            const bool present = valid && std::ranges::find(components_, schemas_.all()[index].id) != components_.end();
            add_.setEnabled(valid && !present && schemas_.all()[index].create);
            remove_.setEnabled(present);
            message_.setText("");
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
        project::ProjectCatalogModel* catalog
    )
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.inspector"}, "Inspector"),
          impl_(std::make_unique<Impl>(*this, sessions, std::move(schemas), std::move(registrations), catalog))
    {}
    InspectorView::~InspectorView() noexcept = default;
    SceneEditResult<void> InspectorView::rebind(EditedSceneBinding binding, SceneObjectRef target)
    {
        return impl_->rebind(binding, target);
    }
    SceneEditResult<void> InspectorView::finishEditing()
    {
        return impl_->finish();
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
        return impl_->clear();
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
        project::ProjectCatalogModel* catalog
    )
    {
        auto view = std::make_unique<InspectorView>(
            dispatcher,
            std::move(id),
            sessions,
            std::move(schemas),
            std::move(registrations),
            catalog
        );
        if (!view->status())
            return cxx::unexpected(SceneViewFailure{view->status().error()});
        auto bound = view->rebind(binding, target);
        if (!bound)
            return cxx::unexpected(SceneViewFailure{bound.error()});
        return views::DetachedView{
            contracts::CodeLease::builtin(),
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
            }
        };
    }
}
