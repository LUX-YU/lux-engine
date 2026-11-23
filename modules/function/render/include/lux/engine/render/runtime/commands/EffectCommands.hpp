#pragma once

#include <lux/engine/render/runtime/JobId.hpp>
#include <lux/engine/render/device/DeviceId.hpp>
#include <lux/engine/render/effect/EffectId.hpp>
#include <lux/engine/render/runtime/commands/Command.hpp>
#include <lux/engine/render/effect/EffectGraph.hpp>
#include <lux/engine/render/runtime/Target.hpp>
#include <memory>
#include <vector>

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
        EffectOutputDesc output{rdesc::ETextureFormat::RGBA8_UNORM};
    };

    // Creates the logical Effect and fixes its output contract without requiring a device.
    struct CreateEffect final
    {
        static constexpr std::string_view name{"render.effect.create"};
        using Request = CreateEffectRequest;
        using Response = EffectCreated;
    };

    struct PrepareEffect final
    {
        static constexpr std::string_view name{"render.effect.prepare"};
        struct Request final
        {
            EffectId effect;
            DeviceSessionId device;
        };
        using Response = AckResponse;
    };

    struct ListEffectJobs final
    {
        static constexpr std::string_view name{"render.effect.jobs"};
        using Request = EffectRequest;
        struct Response final
        {
            RenderError error{};
            std::vector<RenderJobId> jobs;
        };
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
