#include <lux/engine/render/features/FeatureGraphOps.hpp>
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
            return layout;
        }

    } // namespace

    RenderResult<CreateEffectRequest> makeSceneEffect(
        TargetDesc description,
        std::vector<RasterMeshGroup> groups,
        std::uint32_t visible_capacity,
        SceneEffectFeatures features
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
        return input;
    }
} // namespace lux::scene::detail
