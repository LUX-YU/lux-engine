#include <lux/engine/render/capabilities/CapabilityBuild.hpp>
#include <lux/engine/render/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/capabilities/Lighting3dCapability.hpp>

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
        namespace
        {
            constexpr std::array surface_names{"albedo_metallic", "normal_roughness", "emissive_ao", "depth"};
            constexpr std::array light_names{"directional_lights", "point_lights", "spot_lights", "light_counts"};

            RenderResult<CapabilityGraphContribution> lowerLighting(const CapabilityBuildInput& input) noexcept
            {
                const auto& settings = *input.configuration.borrow<DeferredLightingConfiguration>();
                CapabilityGraphContribution result;
                result.ports.resize(input.ports.size());
                auto& graph = result.definition;
                DeferredLightingFeatureConfig lighting;
                lighting.extent = {0, 0, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
                lighting.extent.extent.source = EImageExtentSource::TARGET;
                lighting.environment = settings.environment;
                lighting.input_layout = cameraParameterLayout();
                std::uint32_t shadow_depth = 0;
                if (settings.shadows)
                {
                    auto configuration = ConfigValue::make(settings.shadow);
                    if (!configuration)
                    {
                        return cxx::unexpected(configuration.error());
                    }
                    auto shadows =
                        buildShadowFeatures({input.output, *configuration, {}, {}, input.features.subspan(3, 3)});
                    if (!shadows)
                    {
                        return cxx::unexpected(shadows.error());
                    }
                    lighting.shadows = shadows->features.front().config.borrow<ShadowMapFeatureConfig>()->shape;
                    lighting.external_lights = true;
                    shadow_depth = static_cast<std::uint32_t>(shadows->features.size() - 1);
                    graph = std::move(*shadows);
                }
                std::uint32_t cluster_index = 0;
                if (settings.clustered)
                {
                    ClusteredLightingFeatureConfig clusters;
                    clusters.grid = settings.clusters;
                    clusters.input_layout = cameraParameterLayout();
                    clusters.external_lights = settings.shadows;
                    auto configuration = ConfigValue::make(std::move(clusters));
                    if (!configuration)
                    {
                        return cxx::unexpected(configuration.error());
                    }
                    cluster_index = static_cast<std::uint32_t>(graph.features.size());
                    graph.features.push_back({input.features[2], "clusters", std::move(*configuration)});
                    lighting.clusters = ClusterLightListShape{
                        static_cast<std::uint32_t>(settings.clusters.count()),
                        settings.clusters.index_capacity
                    };
                    if (settings.shadows)
                    {
                        for (auto name : light_names)
                        {
                            graph.connections.push_back({{0, name}, {cluster_index, name}});
                        }
                    }
                }
                auto configuration = ConfigValue::make(std::move(lighting));
                if (!configuration)
                {
                    return cxx::unexpected(configuration.error());
                }
                const auto lighting_index = static_cast<std::uint32_t>(graph.features.size());
                graph.features.push_back({input.features[0], "lighting", std::move(*configuration)});
                for (auto name : surface_names)
                {
                    mapPort(result, input, name, {{lighting_index, name}});
                }
                if (settings.shadows)
                {
                    for (auto [source, destination] :
                         {std::pair{"views", "shadow_views"},
                          {"slices", "shadow_slices"},
                          {"light_ranges", "shadow_ranges"}})
                    {
                        graph.connections.push_back({{0, source}, {lighting_index, destination}});
                    }
                    graph.connections.push_back({{shadow_depth, "depth"}, {lighting_index, "shadow_depth"}});
                    for (auto name : light_names)
                    {
                        graph.connections.push_back({{0, name}, {lighting_index, name}});
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
                    ConfigValue::make(TonemapFeatureConfig{input.output, settings.encoding, cameraParameterLayout()});
                if (!tone)
                {
                    return cxx::unexpected(tone.error());
                }
                const auto display_index = static_cast<std::uint32_t>(graph.features.size());
                graph.features.push_back({input.features[1], "display", std::move(*tone)});
                graph.connections.push_back({{lighting_index, "hdr_color"}, {display_index, "hdr_color"}});
                mapPort(result, input, "color", {{display_index, "color"}});
                return result;
            }
        } // namespace

        CapabilityImplementation DeferredLight::implementation() noexcept
        {
            static const auto result = []
            {
                std::vector<CapabilityPort> ports;
                for (std::size_t i = 0; i < surface_names.size(); ++i)
                {
                    ports.push_back(
                        {surface_names[i],
                         ECapabilityPortDirection::INPUT,
                         std::string(rdesc::GBufferLayout::semantic) + "." + surface_names[i],
                         CapabilityImageShape{
                             i == 3 ? rdesc::GBufferLayout::depth_format : rdesc::GBufferLayout::color_formats[i],
                             1,
                             1
                         },
                         "lux.coordinates.world.forward-z"}
                    );
                }
                ports.push_back(
                    {"color",
                     ECapabilityPortDirection::OUTPUT,
                     "lux.color.display.v1",
                     CapabilityImageShape{std::nullopt, 1, 1},
                     "lux.coordinates.viewport"}
                );
                auto support =
                    describeImplementation(name.name(), kDeferredLightingConfigurationSchema, std::move(ports));
                support.project_parameters = projectCameraInput;
                support.view_parameter_type = cxx::make_basic_type_info<CameraInput>();
                support.decode_configuration =
                    decodeConfigurationValue<DeferredLightingConfiguration, decodeDeferredLightingConfiguration>;
                support.build_graph = lowerLighting;
                return support;
            }();
            return result;
        }
    } // namespace detail
} // namespace lux::render
