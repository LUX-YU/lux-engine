#include "SceneEditPreparation.hpp"

namespace lux::editor::scene::detail
{
    class SceneConfigurationEdit final : public editing::PreparedEdit
    {
    public:
        SceneConfigurationEdit(const SceneEditMemento& edit, bool forward) : edit_(edit), change_(edit.input.change)
        {
            if (edit.input.configuration)
                configuration_ = forward ? *edit.input.configuration : edit.before_configuration;
        }
        editing::EEditEffect effect() const noexcept override
        {
            return edit_.input.configuration ? editing::EEditEffect::CHANGE : editing::EEditEffect::NO_CHANGE;
        }

    private:
        void apply() noexcept override
        {
            if (edit_.input.configuration)
                std::swap(SceneSourceAccess::data(edit_.owner.source).configuration, configuration_);
        }
        void publish(const editing::CommitInfo&) noexcept override
        {
            if (edit_.input.configuration)
                edit_.owner.publish(std::move(change_));
        }
        const SceneEditMemento& edit_;
        SceneConfiguration configuration_;
        SceneChangeRecord change_;
    };
    editing::EditResult<editing::PreparedEditPtr> prepareSceneConfiguration(
        const SceneEditMemento& edit,
        const editing::ApplyContext& context,
        editing::EditPreparationBudget& budget
    )
    {
        auto charged = budget.reserve(sizeof(SceneConfigurationEdit) + edit.input.change.bytes());
        if (!charged)
            return lux::cxx::unexpected(charged.error());
        return editing::PreparedEditPtr(
            std::make_unique<SceneConfigurationEdit>(edit, context.direction == editing::EDirection::FORWARD)
        );
    }
}
