#pragma once

#include <array>
#include <lux/engine/description/GBufferLayout.hpp>
#include <lux/engine/render/commands/Command.hpp>
#include <lux/engine/render/effect/CapabilityDefinition.hpp>
#include <lux/engine/render/effect/ToneMapping.hpp>
#include <lux/engine/render/visibility.h>

namespace lux::render
{
    struct Lighting final
    {
        static constexpr std::string_view capability = "lux.render.lighting";
        static constexpr std::string_view direct = "lux.render.lighting.deferred.direct";
        static constexpr std::string_view clustered = "lux.render.lighting.deferred.clustered";
        static constexpr std::uint32_t version = 1;
        static constexpr auto surface = effectPortSemantic("surface");
        static constexpr auto shadows = effectPortSemantic("shadows");
        static constexpr auto hdr = effectPortSemantic("hdr_color");
        static constexpr auto cluster_lists = effectPortSemantic("cluster_lists");
        static constexpr std::string_view surface_type = rdesc::GBufferLayout::semantic;
        static constexpr std::string_view shadow_type = "lux.lighting.shadow.forward-z";
        static constexpr std::string_view hdr_type = ToneMapping::hdr_type;
        static constexpr std::string_view cluster_lists_type = "lux.lighting.cluster-lists.forward-z";
        static constexpr std::array<std::string_view, 6> cluster_members{
            "grid", "bounds", "counts", "offsets", "indices", "status"
        };
        static constexpr std::array<std::string_view, 4> surface_members{
            "albedo_metallic", "normal_roughness", "emissive_ao", "depth"
        };
        static constexpr std::array<std::string_view, 4> shadow_members{
            "shadow_views", "shadow_slices", "shadow_ranges", "shadow_depth"
        };
        // Each binding selects the corresponding EDirectLightingBuffer projection of a LightResource.
        static constexpr std::array<std::string_view, 4> light_bindings{
            "directional_lights", "point_lights", "spot_lights", "light_counts"
        };

        [[nodiscard]] LUX_RENDER_PUBLIC static CapabilityDefinition definition() noexcept;
    };

    struct SetLightingEnvironment final
    {
        static constexpr std::string_view name{"render.lighting.set_environment"};

        struct Request final
        {
            EffectId effect;
            graph::NodeId node;
            std::array<float, 3> radiance;
        };

        using Response = AckResponse;
        [[nodiscard]] static EffectId effectId(const Request& request) noexcept { return request.effect; }
        [[nodiscard]] static graph::NodeId nodeId(const Request& request) noexcept { return request.node; }
    };
} // namespace lux::render
