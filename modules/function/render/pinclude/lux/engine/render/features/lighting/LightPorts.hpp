#pragma once

#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>
#include <lux/engine/render/extensions/features/FeaturePorts.hpp>
#include <lux/engine/render/resources/lighting/Binding.hpp>

namespace lux::render::detail
{
    inline constexpr std::array kLightPorts{"directional_lights", "point_lights", "spot_lights", "light_counts"};

    inline void describeLightPorts(FeaturePorts& ports, std::string_view resource, bool external) noexcept
    {
        constexpr std::array sizes{
            sizeof(DirectionalLightGpu), sizeof(PointLightGpu), sizeof(SpotLightGpu), sizeof(std::uint32_t) * 4
        };
        for (std::size_t i = 0; i < kLightPorts.size(); ++i)
        {
            const auto bytes = sizes[i];
            if (external)
            {
                ports.resources.push_back({kLightPorts[i], EFeaturePort::INPUT, GraphBufferDesc{bytes}});
            }
            else
            {
                ports.storage.scene.push_back({
                    kLightPorts[i], EFeaturePort::OUTPUT, GraphBufferDesc{bytes}, EGraphOrigin::IMPORTED,
                    {GraphBufferRange{0, bytes}}, ResourceRequirement{ResourceName(resource)}
                });
            }
        }
    }

    inline void requireLightPorts(FeatureBuildContext& context) noexcept
    {
        for (std::size_t i = 0; i < kLightPorts.size(); ++i)
        {
            context.require(StorageArgument::make(
                context.resource(kLightPorts[i]).resource, DirectLightingBinding{static_cast<EDirectLightingBuffer>(i)}
            ));
        }
    }
} // namespace lux::render::detail
