#pragma once

#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/effect/EffectGraph.hpp>
#include <memory>

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
        std::unique_ptr<EffectGraph> graph;
    };

    // Builds a reusable logical recipe. The selected execution compiles its device plan separately.
    struct CreateEffect final
    {
        static constexpr std::string_view name{"render.effect.create"};
        using Request = CreateEffectRequest;
        using Response = EffectCreated;
    };

    struct SetToneMappingExposure final
    {
        static constexpr std::string_view name{"render.tone_mapping.set_exposure"};

        struct Request final
        {
            EffectId effect;
            graph::NodeId node;
            float exposure;
        };

        using Response = AckResponse;
        [[nodiscard]] static EffectId effectId(const Request& request) noexcept { return request.effect; }
        [[nodiscard]] static graph::NodeId nodeId(const Request& request) noexcept { return request.node; }
    };

    struct RemoveEffect final
    {
        static constexpr bool failure_safe = true;
        static constexpr std::string_view name{"render.effect.remove"};
        using Request = EffectRequest;
        using Response = AckResponse;
    };
} // namespace lux::render
