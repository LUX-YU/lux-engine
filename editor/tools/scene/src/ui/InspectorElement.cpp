#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <lux/engine/editor/ui/scene/InspectorElement.hpp>
#include <algorithm>

namespace lux::editor::ui
{
    namespace
    {
        std::vector<lux::ui::ChoiceOption> componentChoices(
            const lux::simulation::ecs::ComponentSchemaSet& schemas,
            const ComponentEditorRegistry& editors,
            std::vector<lux::cxx::TypeToken>& types
        )
        {
            std::vector<lux::ui::ChoiceOption> options;
            for (const auto& schema : schemas.all())
            {
                if (!schema.create)
                    continue;
                const auto* editor = editors.find(schema.cpp_type);
                options.push_back({static_cast<std::int64_t>(types.size()), editor ? editor->name : schema.id.name});
                types.push_back(schema.cpp_type);
            }
            return options;
        }
    }
    InspectorElement::InspectorElement(
        lux::ui::Pane& parent,
        lux::ui::ElementId id,
        const lux::simulation::ecs::ComponentSchemaSet& schemas,
        const ComponentEditorRegistry& editors,
        EditorResult<void>& status,
        const ProjectStorage* catalog
    )
        : lux::ui::Element(parent, std::move(id)), editors_(editors), catalog_(catalog),
          layout_(*this, lux::ui::ElementId{"inspector"}), message_(layout_, lux::ui::ElementId{"message"}),
          creation_layout_(layout_, lux::ui::ElementId{"creation"}, lux::ui::ELayoutType::HORIZONTAL),
          creation_choice_(
              creation_layout_,
              lux::ui::ElementId{"component"},
              componentChoices(schemas, editors, creation_types_)
          ),
          add_component_(creation_layout_, lux::ui::ElementId{"add"}, "Add component"),
          add_connection_(lux::editor::detail::takeConnection(
              lux::object::LuxObject::connect(
                  &add_component_,
                  &lux::ui::Button::activated,
                  [this]() noexcept {
                      const auto index = static_cast<std::size_t>(creation_choice_.value());
                      if (!add_requested_ && index < creation_types_.size())
                          add_requested_.emplace(target_, creation_types_[index]);
                  }
              ),
              status
          ))
    {
        layout_.setScrollable(false, true);
    }

    EditorResult<void> InspectorElement::finishEditing()
    {
        if (!interaction_)
            return {};
        // Finish the control's local interaction as well as its History transaction.
        const auto finish = [&](auto&& self, lux::ui::Element& owner) -> void {
            owner.finishEdit();
            for (auto* child = owner.firstChild(); child; child = child->nextSibling())
                self(self, *static_cast<lux::ui::Element*>(child));
        };
        finish(finish, layout_);
        if (interaction_->finish() && interaction_->finishPending())
            return {};
        const auto& failure = interaction_->failure();
        return lux::cxx::unexpected(EditorFailure{
            failure.code == editing::EEditError::BUSY ? EEditorError::BUSY : EEditorError::INVALID_STATE,
            "inspector.field",
            static_cast<std::uint64_t>(failure.code),
            interaction_->errorMessage(),
            failure
        });
    }

    EditorResult<void> InspectorElement::setTarget(scene::SceneEditing& editing, lux::simulation::ecs::Entity target)
    {
        if (&editing == editing_ && target == target_)
            return {};
        auto finished = finishEditing();
        if (!finished)
            return finished;
        components_.clear();
        add_requested_.reset();
        interaction_.emplace(editing, std::string(id().name()), catalog_);
        editing_ = &editing;
        target_ = target;
        dirty_ = true;
        return {};
    }

    EditorResult<void> InspectorElement::clearTarget()
    {
        const auto finished = finishEditing();
        if (!finished)
            return finished;
        components_.clear();
        add_requested_.reset();
        interaction_.reset();
        editing_ = nullptr;
        target_ = lux::simulation::ecs::NullEntity;
        dirty_ = true;
        return {};
    }

    bool InspectorElement::sameComponents() const noexcept
    {
        std::size_t index{};
        for (const auto& schema : editing_->schemas().all())
        {
            if (schema.semantic_kind == lux::simulation::ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                !editing_->component(target_, schema.cpp_type))
                continue;
            if (index == components_.size() || components_[index++].type != schema.cpp_type)
                return false;
        }
        return index == components_.size();
    }

    void InspectorElement::rebuild()
    {
        // Factories report semantic failures explicitly. Construction remains at the maintenance boundary.
        std::vector<ComponentRow> candidate;
        candidate.reserve(editing_->schemas().all().size());
        for (const auto& schema : editing_->schemas().all())
        {
            if (schema.semantic_kind == lux::simulation::ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                !editing_->component(target_, schema.cpp_type))
                continue;
            const auto found = editors_.find(schema.cpp_type);
            auto& row = candidate.emplace_back();
            row.type = schema.cpp_type;
            if (found != nullptr)
                row.binding = *found;
            const auto identity = schema.id.name;
            row.title = std::make_unique<lux::ui::Label>(
                layout_,
                lux::ui::ElementId{identity + "/title"},
                found == nullptr ? identity : found->name
            );
            if (!row.binding.create)
                continue;
            auto content = row.binding.create(layout_, lux::ui::ElementId{identity}, *editing_, target_, *interaction_);
            if (content)
                row.content = std::move(*content);
            else
                row.title->setText(identity + ": " + content.error().domain + " " + content.error().message);
        }
        components_ = std::move(candidate);
        interaction_->reset();
        dirty_ = false;
    }

    void InspectorElement::update() noexcept
    {
        if (!editing_)
        {
            creation_layout_.setVisible(false);
            message_.setText("No scene is open");
            return;
        }
        if ((!pane().visible() || !pane().focused()) && !finishEditing())
            return;
        if (!interaction_->active() && !interaction_->finishPending())
            return;
        if (add_requested_ && finishEditing())
        {
            const auto [object, type] = *add_requested_;
            if (object != target_)
                add_requested_.reset();
            else
            {
                const auto target = editing_->writeTarget(object);
                auto result = target ? editing_->addComponent(*target, type)
                                     : editing::EditResult<editing::ApplyResult>{lux::cxx::unexpected(target.error())};
                if (result)
                {
                    add_requested_.reset();
                    dirty_ = true;
                }
                else
                {
                    interaction_->fail(result.error());
                    if (result.error().code != editing::EEditError::BUSY)
                        add_requested_.reset();
                }
            }
        }
        if ((dirty_ || !sameComponents()) && finishEditing())
            rebuild();
        const auto target = editing_->writeTarget(target_);
        const auto index = static_cast<std::size_t>(creation_choice_.value());
        add_component_.setEnabled(
            !add_requested_ && index < creation_types_.size() && target &&
            bool(editing_->canAddComponent(*target, creation_types_[index]))
        );
        creation_layout_.setVisible(!creation_types_.empty());
        if (interaction_->errorMessage()[0])
            message_.setText(interaction_->errorMessage());
        else if (target_ == lux::simulation::ecs::NullEntity)
            message_.setText("Select an object");
        else
            message_.setText(std::string(editing_->writeRestriction()));
    }
    lux::ui::SizeHint InspectorElement::sizeHintContent() noexcept
    {
        return layout_.sizeHint();
    }
    lux::ui::SizeHint InspectorElement::measureContent(float width) noexcept
    {
        return layout_.measure(width);
    }
    void InspectorElement::arrangeContent() noexcept
    {
        layout_.arrange({{}, rect().size});
    }
    void InspectorElement::draw() noexcept
    {
        drawChild(layout_);
    }

}
