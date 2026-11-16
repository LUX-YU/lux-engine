#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/capabilities/CapabilityBuild.hpp>
#include <lux/engine/render/capabilities/Mesh3dCapability.hpp>
#include <lux/engine/render/capabilities/RasterSurface.hpp>

namespace lux::render
{
    namespace
    {
        auto fields(auto& value) noexcept
        {
            return std::tie(value.visibility);
        }

        RenderResult<CapabilityGraphContribution> lowerSurface(const CapabilityBuildInput& input) noexcept
        {
            const auto& options = *static_cast<const RasterSurfaceConfiguration*>(input.configuration.data());
            GBufferFeatureConfig surface{input.output};
            surface.input_layout = detail::cameraParameterLayout();
            MeshVisibilityFeatureConfig visibility;
            visibility.frustum_culling = options.visibility;
            visibility.view_parameters = surface.input_layout;
            auto visible = ConfigValue::make(std::move(visibility));
            if (!visible)
            {
                return cxx::unexpected(visible.error());
            }
            CapabilityGraphContribution result;
            result.ports.resize(input.ports.size());
            result.definition.features.push_back({input.features[1], "visibility", std::move(*visible)});
            for (const auto port : {"groups", "transforms", "properties", "alive", "visible", "commands"})
            {
                result.definition.connections.push_back({{0, port}, {1, port}});
            }
            const auto raster = static_cast<std::uint32_t>(result.definition.features.size());
            auto value = ConfigValue::make(std::move(surface));
            if (!value)
            {
                return cxx::unexpected(value.error());
            }
            result.definition.features.push_back({input.features[0], "surface", std::move(*value)});
            for (const auto& port : input.ports)
            {
                detail::mapPort(result, input, port.key, {{raster, port.key}});
            }
            return result;
        }
    } // namespace

    RenderResult<SerializedRenderConfiguration>
    encodeRasterSurfaceConfiguration(const RasterSurfaceConfiguration& value) noexcept
    {
        return detail::encodeConfiguration(
            kRasterSurfaceConfigurationSchema,
            value,
            [](const auto& v) { return fields(v); }
        );
    }

    RenderResult<RasterSurfaceConfiguration> decodeRasterSurfaceConfiguration(std::span<const std::byte> bytes) noexcept
    {
        return detail::decodeConfiguration<RasterSurfaceConfiguration>(bytes, [](auto& v) { return fields(v); });
    }

    namespace detail
    {
        CapabilityImplementation RasterSurface::implementation() noexcept
        {
            static const auto result = []
            {
                std::vector<CapabilityPort> ports;
                const std::array names{"albedo_metallic", "normal_roughness", "emissive_ao", "depth"};
                for (std::size_t i = 0; i < names.size(); ++i)
                {
                    ports.push_back(
                        {names[i],
                         ECapabilityPortDirection::OUTPUT,
                         std::string(rdesc::GBufferLayout::semantic) + "." + names[i],
                         CapabilityImageShape{
                             i == 3 ? rdesc::GBufferLayout::depth_format : rdesc::GBufferLayout::color_formats[i],
                             1,
                             1
                         },
                         "lux.coordinates.world.forward-z"}
                    );
                }
                auto support = describeImplementation(name.name(), kRasterSurfaceConfigurationSchema, std::move(ports));
                support.resource_dependencies = rasterStorageDependencies;
                support.project_parameters = projectCameraInput;
                support.view_parameter_type = cxx::make_basic_type_info<CameraInput>();
                support.decode_configuration =
                    decodeConfigurationValue<RasterSurfaceConfiguration, decodeRasterSurfaceConfiguration>;
                support.build_graph = lowerSurface;
                return support;
            }();
            return result;
        }
    } // namespace detail
} // namespace lux::render
