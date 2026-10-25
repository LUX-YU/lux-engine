#pragma once

#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/extensions/features/EffectDefinition.hpp>

namespace lux::render
{
    // Explicit graph-author entry. Ordinary applications select a capability through CreateEffect.
    // Both requests use the same compiler, Effect directory and asynchronous adoption path.
    struct GraphEffectRequest final
    {
        TargetDesc description;
        EffectDefinition definition;
    };

    struct CreateGraphEffect final
    {
        static constexpr std::string_view name = "render.effect.create_graph";
        static constexpr bool frame_scoped = false;
        using Request = GraphEffectRequest;
        using Response = EffectCreated;
    };
} // namespace lux::render
