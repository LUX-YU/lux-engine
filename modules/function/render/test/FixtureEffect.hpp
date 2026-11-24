#pragma once

#include <lux/engine/render/effect/EffectImplementation.hpp>

namespace lux::render::fixture
{
    template <auto Build> class Effect : public EffectImplementation
    {
    public:
        explicit Effect(std::string_view arguments)
            : EffectImplementation(arguments.size()), arguments_(arguments) {}

        static EffectCreateResult create(graph::NodeId, std::string_view arguments) noexcept
        {
            return std::make_unique<Effect>(arguments);
        }

        RenderResult<void> onBuild(EffectCompileContext& context) const noexcept override { return Build(context, arguments_); }

    private:
        std::string arguments_;
    };

    template <auto Build, class State> class StatefulEffect final : public Effect<Build>
    {
    public:
        StatefulEffect(std::string_view arguments, State state) : Effect<Build>(arguments), state_(std::move(state))
        {
            this->bindState(state_);
        }

    private:
        State state_;
    };
}
