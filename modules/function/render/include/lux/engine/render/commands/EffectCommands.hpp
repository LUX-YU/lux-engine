#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/core/EffectParameters.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/extensions/features/EffectDefinition.hpp>

namespace lux::render
{
    struct EffectRequest final
    {
        EffectId effect{};
    };

    struct EffectCreated final
    {
        RenderError error{};
        EffectId effect{};
    };

    struct EffectImplementationUse final
    {
        CapabilityImplementationId implementation;
        std::string contribution;
    };

    struct CreateEffectRequest final
    {
        EffectOutputDesc output;
        EffectDefinition definition;
        // With selections, each Feature key names exactly one implementation contribution or its slash-delimited
        // child. Live external Resources must belong to the union of selected Capabilities.
        // With no selections, the Feature ports declare the free graph's Resource requirements.
        std::vector<EffectImplementationUse> implementations;
        EffectParameters parameters;
    };

    // Builds a reusable logical recipe. The selected execution compiles its device plan separately.
    struct CreateEffect final
    {
        static constexpr std::string_view name{"render.effect.create"};
        using Request = CreateEffectRequest;
        using Response = EffectCreated;
    };

    // Updates every execution of this Effect at the next Prepare. View and execution-local values are separate.
    struct SetEffectParameters final
    {
        static constexpr std::string_view name{"render.effect.set_parameters"};

        struct Request final
        {
            EffectId effect;
            EffectParameters parameters;
        };

        using Response = AckResponse;
    };

    struct RemoveEffect final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.effect.remove"};
        using Request = EffectRequest;
        using Response = AckResponse;
    };
} // namespace lux::render
