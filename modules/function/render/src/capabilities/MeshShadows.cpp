#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/capabilities/CapabilityBuild.hpp>
#include <lux/engine/render/capabilities/MeshShadows.hpp>
#include <lux/engine/render/commands/Lighting3dCommands.hpp>

#include <cmath>

namespace lux::render
{
    namespace detail
    {
        RenderResult<EffectDefinition> buildShadowFeatures(const CapabilityBuildInput& input) noexcept
        {
            const auto& configuration = *static_cast<const MeshShadowsConfiguration*>(input.configuration.data());
            ShadowMapFeatureConfig projection;
            projection.shape.resolution = configuration.resolution;
            projection.camera_offset = offsetof(CameraParameters, inverse_view_projection);
            projection.pcf_radius = configuration.filter_radius;
            projection.distance = configuration.distance;
            MeshShadowFeatureConfig shadow{projection.shape};
            EffectDefinition result;
            auto project = ConfigValue::make(std::move(projection));
            if (!project)
            {
                return cxx::unexpected(project.error());
            }
            result.features.push_back({input.features[0], "projection", std::move(*project)});
            MeshVisibilityFeatureConfig visible;
            visible.view_parameters.reset();
            visible.shadow_pass = true;
            auto visibility = ConfigValue::make(std::move(visible));
            if (!visibility)
            {
                return cxx::unexpected(visibility.error());
            }
            result.features.push_back({input.features[2], "visibility", std::move(*visibility)});
            result.connections.push_back({{0, "views"}, {1, "views"}});
            for (const auto port : {"groups", "transforms", "properties", "alive", "visible", "commands"})
            {
                result.connections.push_back({{1, port}, {2, port}});
            }
            const auto raster = static_cast<std::uint32_t>(result.features.size());
            auto mesh = ConfigValue::make(std::move(shadow));
            if (!mesh)
            {
                return cxx::unexpected(mesh.error());
            }
            result.features.push_back({input.features[1], "depth", std::move(*mesh)});
            result.connections.push_back({{0, "views"}, {raster, "views"}});
            result.connections.push_back({{0, "raster_states"}, {raster, "raster_states"}});
            return result;
        }
    } // namespace detail
} // namespace lux::render
