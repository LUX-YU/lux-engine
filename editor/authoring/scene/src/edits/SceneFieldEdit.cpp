#include "SceneEditPreparation.hpp"

namespace lux::editor::scene::detail
{
    namespace ecs = simulation::ecs;
    class SceneFieldEdit final : public editing::PreparedEdit
    {
    public:
        explicit SceneFieldEdit(const SceneEditMemento& edit, bool forward)
            : edit_(edit),
              configuration_(
                  edit.input.configuration ? (forward ? *edit.input.configuration : edit.before_configuration)
                                           : SceneConfiguration{}
              ),
              change_(edit.input.change)
        {}
        [[nodiscard]] editing::EditResult<void> prepare(bool forward, editing::EditPreparationBudget& budget)
        {
            const auto& source = SceneSourceAccess::data(edit_.owner.source);
            for (const auto& field : edit_.input.fields)
            {
                const auto entity = source.identities.entity(field.object);
                const auto* schema = source.schemas.find(field.before.schema);
                const bool is_invalid_target = entity == ecs::NullEntity || !schema || !field.swap;
                if (is_invalid_target)
                    return lux::cxx::unexpected(editFailure(SceneEditError{ESceneEditError::INVALID_COMPONENT}));
                const auto& value = forward ? field.after : field.before;
                auto charged = budget.reserve(value.bytes.size() + schema->operations.valueBytes());
                if (!charged)
                    return charged;
                const auto scratch_entity = scratch_.create();
                entities_.push_back(scratch_entity);
                auto decoded = decodeInto(scratch_, scratch_entity, value, *schema, source.identities);
                if (!decoded)
                    return lux::cxx::unexpected(editFailure(decoded.error()));
            }
            return budget.reserve(sizeof(*this) + change_.bytes());
        }
        editing::EEditEffect effect() const noexcept override
        {
            return editing::EEditEffect::CHANGE;
        }

    private:
        void apply() noexcept override
        {
            auto& source = SceneSourceAccess::data(edit_.owner.source);
            for (std::size_t index{}; index < edit_.input.fields.size(); ++index)
            {
                const auto& field = edit_.input.fields[index];
                const auto entity = source.identities.entity(field.object);
                field.swap(source.registry, entity, scratch_, entities_[index]);
            }
            if (edit_.input.configuration)
                std::swap(source.configuration, configuration_);
        }
        void publish(const editing::CommitInfo&) noexcept override
        {
            auto& source = SceneSourceAccess::data(edit_.owner.source);
            for (const auto& field : edit_.input.fields)
                source.schemas.find(field.before.schema)
                    ->operations.notifyUpdated(source.registry, source.identities.entity(field.object));
            edit_.owner.publish(std::move(change_));
        }
        const SceneEditMemento& edit_;
        ecs::Registry scratch_;
        std::vector<ecs::Entity> entities_;
        SceneConfiguration configuration_;
        SceneChangeRecord change_;
    };

    editing::EditResult<editing::PreparedEditPtr> prepareSceneFields(
        const SceneEditMemento& edit,
        const editing::ApplyContext& context,
        editing::EditPreparationBudget& budget
    )
    {
        const bool forward = context.direction == editing::EDirection::FORWARD;
        auto result = std::make_unique<SceneFieldEdit>(edit, forward);
        if (auto prepared = result->prepare(forward, budget); !prepared)
            return lux::cxx::unexpected(prepared.error());
        return editing::PreparedEditPtr(std::move(result));
    }
}
