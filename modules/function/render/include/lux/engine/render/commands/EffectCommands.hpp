#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/capabilities/CapabilityComposition.hpp>
#include <lux/engine/render/core/Target.hpp>

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

    struct CreateEffectRequest final
    {
        EffectOutputDesc output;
        CapabilityComposition composition;
    };

    struct CreateEffect final
    {
        static constexpr std::string_view name{"render.effect.create"};
        static constexpr bool requires_frame_boundary = false;
        using Request = CreateEffectRequest;
        using Response = EffectCreated;
    };

    struct RemoveEffect final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.effect.remove"};
        static constexpr bool requires_frame_boundary = true;
        using Request = EffectRequest;
        using Response = AckResponse;
    };
} // namespace lux::render
