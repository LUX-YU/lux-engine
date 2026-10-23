#include <lux/engine/render/features/FeatureGraphOps.hpp>
#include <lux/engine/render/features/MeshShadowFeature.hpp>
#include <lux/engine/render/features/MeshVisibilityFeature.hpp>
#include <lux/engine/scene/SceneEffect.hpp>

namespace lux::scene::detail
{
    using namespace render;

    namespace
    {
        rdesc::ShaderBlock sceneParameterLayout() noexcept
        {
            auto layout = meshViewParameterBlock();
            layout.size = sizeof(SceneParameters);
            for (auto field : deferredLightingParameterBlock().fields)
            {
                field.offset += offsetof(SceneParameters, lighting);
                layout.fields.push_back(std::move(field));
            }
            auto exposure = tonemapParameterBlock().fields.front();
            exposure.offset += offsetof(SceneParameters, tone);
            layout.fields.push_back(std::move(exposure));
            for (auto field : clusteredLightingParameterBlock().fields)
            {
                field.offset += offsetof(SceneParameters, clusters);
                layout.fields.push_back(std::move(field));
            }
            return layout;
        }

    } // namespace

    RenderResult<CreateEffectRequest> makeSceneEffect(
        TargetDesc description,
        std::vector<RasterMeshGroup> groups,
        std::uint32_t visible_capacity,
        SceneEffectFeatures features,
        const RenderSystemConfiguration& configuration,
        std::vector<ShadowLightSelection> casters
    ) noexcept
    {
        const auto layout = sceneParameterLayout();
        GBufferFeatureConfig surface{description};
        surface.input_layout = layout;
        surface.groups = std::move(groups);
        surface.visible_capacity = visible_capacity;
        DeferredLightingFeatureConfig light;
        light.extent = {description.width, description.height, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
        light.extent.extent.source = EImageExtentSource::TARGET;
        light.input_layout = layout;
        const bool has_shadows = configuration.shadows && !casters.empty() && !surface.groups.empty();
        const bool has_clusters = configuration.recipe == "lux.scene.clustered";
        ShadowMapFeatureConfig projection;
        projection.shape = {configuration.shadow_resolution, 0};
        projection.camera_offset = offsetof(SceneParameters, shadow);
        projection.pcf_radius = configuration.shadow_filter_radius;
        projection.casters = std::move(casters);
        for (const auto& caster : projection.casters)
        {
            projection.shape.view_count += caster.kind == EDirectLightingBuffer::POINT ? 6
                                           : caster.kind == EDirectLightingBuffer::SPOT
                                               ? 1
                                               : static_cast<std::uint32_t>(caster.cascade_far_distances.size());
        }
        ClusteredLightingFeatureConfig clusters;
        clusters.grid.index_capacity = configuration.cluster_index_capacity;
        clusters.external_lights = has_shadows;
        clusters.input_layout = layout;
        if (has_shadows)
        {
            light.shadows = projection.shape;
        }
        if (has_clusters)
        {
            light.clusters = clusters.grid;
        }
        const auto encoding = EColorEncoding::SRGB;
        TonemapFeatureConfig tone{description, encoding, layout};
        MeshVisibilityFeatureConfig selection;
        selection.candidate_capacity = surface.visible_capacity;
        selection.view_parameters = layout;
        selection.mesh_storage = "assets";
        for (const auto& group : surface.groups)
        {
            selection.groups.push_back(group.group);
        }
        CreateEffectRequest input{description};
        const auto group_count = selection.groups.size();
        // Shadow and main visibility share the Scene instances, with independent per-View outputs.
        MeshShadowFeatureConfig shadow{projection.shape, surface.groups, visible_capacity};
        auto shadow_selection = selection;
        shadow_selection.view_count = projection.shape.view_count;
        shadow_selection.view_parameters.reset();
        auto surface_config = ConfigValue::make(std::move(surface));
        auto light_config = ConfigValue::make(std::move(light));
        auto tone_config = ConfigValue::make(std::move(tone));
        auto visibility_config = ConfigValue::make(std::move(selection));
        if (!surface_config || !light_config || !tone_config || !visibility_config)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        input.definition.features.push_back({features.gbuffer, "surfaces", std::move(*surface_config)});
        input.definition.features.push_back({features.lighting, "lighting", std::move(*light_config)});
        input.definition.features.push_back({features.tonemap, "tonemap", std::move(*tone_config)});
        for (auto name : {"albedo_metallic", "normal_roughness", "emissive_ao", "depth"})
        {
            input.definition.connections.push_back({{0, name}, {1, name}});
        }
        input.definition.connections.push_back({{1, "hdr_color"}, {2, "hdr_color"}});
        input.definition.outputs.push_back({{2, "color"}, GraphOutputRequirements{description.format, 1}});
        if (group_count != 0)
        {
            input.definition.features.push_back({features.visibility, "visibility", std::move(*visibility_config)});
            for (std::size_t i = 0; i < group_count; ++i)
            {
                const auto from = "g" + std::to_string(i) + ".";
                const auto to = "group" + std::to_string(i) + ".";
                for (auto [source, destination] :
                     {std::pair{"visible", "members"}, {"commands", "commands"}, {"counts", "counts"}})
                {
                    input.definition.connections.push_back({{3, from + source}, {0, to + destination}});
                }
            }
        }
        if (has_shadows)
        {
            const auto projection_index = static_cast<std::uint32_t>(input.definition.features.size());
            const auto visibility_index = projection_index + 1;
            const auto shadow_index = projection_index + 2;
            auto projection_config = ConfigValue::make(std::move(projection));
            auto selection_config = ConfigValue::make(std::move(shadow_selection));
            auto shadow_config = ConfigValue::make(std::move(shadow));
            if (!projection_config || !selection_config || !shadow_config)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            input.definition.features.push_back({features.shadows, "shadow_projection", std::move(*projection_config)});
            input.definition.features.push_back({features.visibility, "shadow_visibility", std::move(*selection_config)}
            );
            input.definition.features.push_back({features.mesh_shadow, "shadow_depth", std::move(*shadow_config)});
            for (auto name : {"directional_lights", "point_lights", "spot_lights", "light_counts"})
            {
                input.definition.connections.push_back({{projection_index, name}, {1, name}});
            }
            auto& connections = input.definition.connections;
            connections.push_back({{projection_index, "views"}, {visibility_index, "views"}});
            connections.push_back({{projection_index, "views"}, {shadow_index, "views"}});
            connections.push_back({{projection_index, "raster_states"}, {shadow_index, "raster_states"}});
            connections.push_back({{projection_index, "views"}, {1, "shadow_views"}});
            connections.push_back({{projection_index, "slices"}, {1, "shadow_slices"}});
            connections.push_back({{projection_index, "light_ranges"}, {1, "shadow_ranges"}});
            connections.push_back({{shadow_index, "depth"}, {1, "shadow_depth"}});
            for (std::size_t i = 0; i < group_count; ++i)
            {
                const auto prefix = "g" + std::to_string(i) + ".";
                for (auto [source, destination] :
                     {std::pair{"visible", "members"}, {"commands", "commands"}, {"counts", "counts"}})
                {
                    connections.push_back({{visibility_index, prefix + source}, {shadow_index, prefix + destination}});
                }
            }
            if (has_clusters)
            {
                const auto cluster_index = static_cast<std::uint32_t>(input.definition.features.size());
                for (auto name : {"directional_lights", "point_lights", "spot_lights", "light_counts"})
                {
                    connections.push_back({{projection_index, name}, {cluster_index, name}});
                }
            }
        }
        if (has_clusters)
        {
            const auto cluster_index = static_cast<std::uint32_t>(input.definition.features.size());
            auto cluster_config = ConfigValue::make(std::move(clusters));
            if (!cluster_config)
            {
                return cxx::unexpected(cluster_config.error());
            }
            input.definition.features.push_back({features.clusters, "clusters", std::move(*cluster_config)});
            if (!has_shadows)
            {
                for (auto name : {"directional_lights", "point_lights", "spot_lights", "light_counts"})
                {
                    input.definition.connections.push_back({{cluster_index, name}, {1, name}});
                }
            }
            for (auto name : {"grid", "offsets", "indices", "status"})
            {
                input.definition.connections.push_back({{cluster_index, name}, {1, "cluster_" + std::string(name)}});
            }
        }
        return input;
    }
} // namespace lux::scene::detail
