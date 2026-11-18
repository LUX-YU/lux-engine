#include <lux/engine/render/capabilities/CapabilityBuild.hpp>
#include <lux/engine/render/capabilities/FeatureRecipes.hpp>
#include <lux/engine/render/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/capabilities/Lighting3dCapability.hpp>
#include <lux/engine/render/features/lighting/LightPorts.hpp>

#include <cmath>

namespace lux::render
{
    bool DeferredLightingConfiguration::valid() const noexcept
    {
        for (const float value : environment)
        {
            if (!std::isfinite(value) || value < 0)
            {
                return false;
            }
        }
        return shadow.valid() && clusters.valid() && encoding <= EColorEncoding::SRGB;
    }

    RenderResult<SerializedRenderConfiguration>
    encodeDeferredLightingConfiguration(const DeferredLightingConfiguration& value) noexcept
    {
        if (!value.valid())
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        return detail::encodeConfiguration(
            kDeferredLightingConfigurationSchema,
            value,
            [](const auto& v)
            {
                return std::tie(
                    v.environment,
                    v.shadows,
                    v.clustered,
                    v.shadow.resolution,
                    v.shadow.filter_radius,
                    v.shadow.distance,
                    v.clusters.cells,
                    v.clusters.index_capacity,
                    v.clusters.depth,
                    v.encoding
                );
            }
        );
    }

    RenderResult<DeferredLightingConfiguration>
    decodeDeferredLightingConfiguration(std::span<const std::byte> bytes) noexcept
    {
        auto result = detail::decodeConfiguration<DeferredLightingConfiguration>(
            bytes,
            [](auto& v)
            {
                return std::tie(
                    v.environment,
                    v.shadows,
                    v.clustered,
                    v.shadow.resolution,
                    v.shadow.filter_radius,
                    v.shadow.distance,
                    v.clusters.cells,
                    v.clusters.index_capacity,
                    v.clusters.depth,
                    v.encoding
                );
            }
        );
        if (!result || !result->valid())
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        return result;
    }

    namespace detail
    {
        RenderResult<LightingPorts>
        appendLightingFeatures(
            EffectDefinition& destination,
            std::span<const FeatureId> features,
            const EffectOutputDesc& output,
            const DeferredLightingConfiguration& settings,
            std::string_view key
        ) noexcept
        {
            if (!validRecipeFeatures(features, 6)) return cxx::unexpected(RenderError{kFeatureContract});
            if (!settings.valid()) return cxx::unexpected(RenderError{kInvalidConfig});
            EffectDefinition graph;
            DeferredLightingFeatureConfig lighting;
            lighting.extent = {0, 0, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
            lighting.extent.extent.source = EImageExtentSource::TARGET;
            lighting.environment = settings.environment;
            std::optional<ShadowPorts> shadows;
            if (settings.shadows)
            {
                auto ports = appendShadowFeatures(graph, features.subspan(3, 3), settings.shadow);
                if (!ports)
                {
                    return cxx::unexpected(ports.error());
                }
                lighting.shadows = ports->shape;
                lighting.external_lights = true;
                shadows = std::move(*ports);
            }
            std::uint32_t cluster_index = 0;
            if (settings.clustered)
            {
                ClusteredLightingFeatureConfig clusters;
                clusters.grid = settings.clusters;
                clusters.external_lights = settings.shadows;
                auto configuration = ConfigValue::make(std::move(clusters));
                if (!configuration)
                {
                    return cxx::unexpected(configuration.error());
                }
                cluster_index = static_cast<std::uint32_t>(graph.features.size());
                graph.features.push_back({features[2], "clusters", std::move(*configuration)});
                lighting.clusters = ClusterLightListShape{
                    static_cast<std::uint32_t>(settings.clusters.count()),
                    settings.clusters.index_capacity
                };
                if (shadows)
                {
                    for (auto name : kLightPorts)
                    {
                        graph.connections.push_back({{shadows->projection, name}, {cluster_index, name}});
                    }
                }
            }
            auto configuration = ConfigValue::make(std::move(lighting));
            if (!configuration)
            {
                return cxx::unexpected(configuration.error());
            }
            const auto lighting_index = static_cast<std::uint32_t>(graph.features.size());
            graph.features.push_back({features[0], "lighting", std::move(*configuration)});
            if (shadows)
            {
                for (auto [source, destination] :
                     {std::pair{"views", "shadow_views"},
                      {"slices", "shadow_slices"},
                      {"light_ranges", "shadow_ranges"}})
                {
                    graph.connections.push_back({{shadows->projection, source}, {lighting_index, destination}});
                }
                graph.connections.push_back({shadows->depth, {lighting_index, "shadow_depth"}});
                for (auto name : kLightPorts)
                {
                    graph.connections.push_back({{shadows->projection, name}, {lighting_index, name}});
                }
            }
            if (settings.clustered)
            {
                for (auto name : {"grid", "offsets", "indices", "status"})
                {
                    graph.connections.push_back(
                        {{cluster_index, name}, {lighting_index, "cluster_" + std::string(name)}}
                    );
                }
            }
            auto tone =
                ConfigValue::make(TonemapFeatureConfig{output, settings.encoding});
            if (!tone)
            {
                return cxx::unexpected(tone.error());
            }
            const auto display_index = static_cast<std::uint32_t>(graph.features.size());
            graph.features.push_back({features[1], "display", std::move(*tone)});
            graph.connections.push_back({{lighting_index, "hdr_color"}, {display_index, "hdr_color"}});
            auto offset = appendFeatureRecipe(destination, std::move(graph), key);
            if (!offset) return cxx::unexpected(offset.error());
            LightingPorts ports;
            for (std::size_t i = 0; i < kGBufferPorts.size(); ++i)
                ports.surface[i] = {*offset + lighting_index, std::string(kGBufferPorts[i])};
            ports.color = {*offset + display_index, "color"};
            return ports;
        }

        CapabilityImplementation DeferredLight::implementation() noexcept
        {
            auto result = describeImplementation(name.name());
            return result;
        }
    } // namespace detail
} // namespace lux::render
