#include <lux/engine/render/capabilities/FeatureRecipes.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityFeature.hpp>
#include <lux/engine/render/features/shadow/MeshShadowFeature.hpp>
#include <lux/engine/render/features/shadow/ShadowMapFeature.hpp>

namespace lux::render
{
    namespace detail
    {
        RenderResult<ShadowPorts>
        appendShadowFeatures(
            EffectDefinition& graph,
            std::span<const FeatureId> features,
            const MeshShadowsConfiguration& configuration
        ) noexcept
        {
            if (!validRecipeFeatures(features, 3)) return cxx::unexpected(RenderError{kFeatureContract});
            const bool invalid_recipe = !configuration.valid() || graph.features.size() > UINT32_MAX - features.size();
            if (invalid_recipe) return cxx::unexpected(RenderError{kInvalidConfig});
            ShadowMapFeatureConfig projection;
            projection.shape.resolution = configuration.resolution;
            projection.pcf_radius = configuration.filter_radius;
            projection.distance = configuration.distance;
            MeshShadowFeatureConfig shadow{projection.shape};
            const auto shape = projection.shape;
            auto project = ConfigValue::make(std::move(projection));
            if (!project)
            {
                return cxx::unexpected(project.error());
            }
            MeshVisibilityFeatureConfig visible;
            visible.shadow_pass = true;
            auto visibility = ConfigValue::make(std::move(visible));
            if (!visibility)
            {
                return cxx::unexpected(visibility.error());
            }
            auto mesh = ConfigValue::make(std::move(shadow));
            if (!mesh)
            {
                return cxx::unexpected(mesh.error());
            }
            const auto first = static_cast<std::uint32_t>(graph.features.size());
            const auto culling = first + 1;
            const auto raster = first + 2;
            graph.features.push_back({features[0], "projection", std::move(*project)});
            graph.features.push_back({features[2], "visibility", std::move(*visibility)});
            graph.features.push_back({features[1], "depth", std::move(*mesh)});
            graph.connections.push_back({{first, "views"}, {culling, "views"}});
            for (const auto port : kMeshDrawInputPorts)
            {
                graph.connections.push_back({{culling, port}, {raster, port}});
            }
            graph.connections.push_back({{first, "views"}, {raster, "views"}});
            graph.connections.push_back({{first, "raster_states"}, {raster, "raster_states"}});
            return ShadowPorts{shape, first, {raster, "depth"}};
        }
    } // namespace detail
} // namespace lux::render
