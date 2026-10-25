#include <cmath>
#include <cstring>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/capabilities/RasterAssets.hpp>
#include <lux/engine/render/extensions/GraphCommands.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/LitSceneParameters.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>
#include <ranges>

namespace lux::render::detail
{
    struct SceneFeatures final
    {
        FeatureId gbuffer, lighting, tonemap, visibility, shadows, mesh_shadow, clusters;
    };

    namespace
    {
        bool validConfiguration(const LitSceneConfiguration& value) noexcept
        {
            const bool valid_resolution =
                value.shadow_filter_radius <= 4 && value.shadow_resolution > 2 * (value.shadow_filter_radius + 2);
            const bool valid_distance = std::isfinite(value.shadow_distance) && value.shadow_distance > 0;
            return valid_resolution && valid_distance && value.cluster_index_capacity != 0;
        }

        auto configurationFields(auto& value) noexcept
        {
            return std::tie(
                value.shadows,
                value.shadow_resolution,
                value.shadow_filter_radius,
                value.shadow_distance,
                value.cluster_index_capacity
            );
        }

        RenderResult<ConfigValue> decodeConfiguration(std::span<const std::byte> bytes) noexcept
        {
            auto value = decodeLitSceneConfiguration(bytes);
            if (!value)
            {
                return cxx::unexpected(value.error());
            }
            return ConfigValue::make(*value);
        }

        rdesc::ShaderBlock sceneParameterLayout() noexcept
        {
            auto layout = meshViewParameterBlock();
            layout.size = sizeof(LitSceneParameters);
            for (auto field : deferredLightingParameterBlock().fields)
            {
                field.offset += offsetof(LitSceneParameters, lighting);
                layout.fields.push_back(std::move(field));
            }
            auto exposure = tonemapParameterBlock().fields.front();
            exposure.offset += offsetof(LitSceneParameters, tone);
            layout.fields.push_back(std::move(exposure));
            for (auto field : clusteredLightingParameterBlock().fields)
            {
                field.offset += offsetof(LitSceneParameters, clusters);
                layout.fields.push_back(std::move(field));
            }
            return layout;
        }

    } // namespace

    RenderResult<GraphEffectRequest> makeSceneGraph(
        TargetDesc description,
        std::vector<RasterMeshGroup> groups,
        std::uint32_t visible_capacity,
        SceneFeatures features,
        const LitSceneConfiguration& configuration,
        bool clustered,
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
        const bool has_clusters = clustered;
        ShadowMapFeatureConfig projection;
        projection.shape = {configuration.shadow_resolution, 0};
        projection.camera_offset = offsetof(LitSceneParameters, shadow);
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
        GraphEffectRequest input{description};
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
} // namespace lux::render::detail

namespace lux::render::detail
{
    namespace
    {
        RenderResult<std::vector<ShadowLightSelection>> selectShadows(
            const LitSceneInputs& input,
            const LitSceneConfiguration& configuration
        ) noexcept
        {
            std::vector<ShadowLightSelection> result;
            if (!configuration.shadows)
            {
                return result;
            }
            const auto near_distance = input.near_distance;
            const auto far_distance = std::min(input.far_distance, configuration.shadow_distance);
            for (const auto& source : input.lights)
            {
                const auto& light = source.description;
                if (!light.cast_shadow)
                {
                    continue;
                }
                const bool invalid_count =
                    light.cascade_count == 0 || light.cascade_count > light.cascade_splits.size();
                const bool invalid_clip = !std::isfinite(near_distance) || !std::isfinite(far_distance) ||
                                          near_distance <= 0 || far_distance <= near_distance;
                if (light.type == rdesc::ELightType::DIRECTIONAL && (invalid_count || invalid_clip))
                {
                    return cxx::unexpected(RenderError{kCommandContract});
                }
                ShadowLightSelection selected;
                selected.light = source.light;
                selected.kind = light.type == rdesc::ELightType::DIRECTIONAL ? EDirectLightingBuffer::DIRECTIONAL
                                : light.type == rdesc::ELightType::POINT     ? EDirectLightingBuffer::POINT
                                                                             : EDirectLightingBuffer::SPOT;
                selected.normal_offset = light.shadow_normal_bias;
                selected.comparison_offset = light.shadow_bias;
                selected.caster_distance = configuration.shadow_distance;
                if (selected.kind == EDirectLightingBuffer::DIRECTIONAL)
                {
                    selected.cascade_far_distances.clear();
                    for (std::uint32_t i = 0; i < light.cascade_count; ++i)
                    {
                        const float split = light.cascade_splits[i];
                        const bool invalid_split = !std::isfinite(split) || split <= 0 || split > 1 ||
                                                   (i != 0 && split <= light.cascade_splits[i - 1]);
                        if (invalid_split)
                        {
                            return cxx::unexpected(RenderError{kCommandContract});
                        }
                        selected.cascade_far_distances.push_back(
                            near_distance + (far_distance - near_distance) * split
                        );
                    }
                }
                result.push_back(std::move(selected));
            }
            return result;
        }

        template <bool Clustered>
        RenderResult<void> expandScene(
            const TargetDesc& target,
            const void* configuration,
            const void* data,
            std::span<const FeatureId> dependencies,
            EffectDefinition& definition
        ) noexcept
        {
            const auto& settings = *static_cast<const LitSceneConfiguration*>(configuration);
            const auto& input = *static_cast<const LitSceneInputs*>(data);
            std::vector<RasterMeshGroup> groups;
            groups.reserve(input.geometry.size());
            for (const auto& source : input.geometry)
            {
                RasterMeshGroup group{
                    source.geometry,
                    source.program,
                    {source.selection.mesh, source.selection.material, source.selection.submesh}
                };
                group.mesh_storage = "assets";
                group.material_storage = "assets";
                group.front_counter_clockwise = source.front_counter_clockwise;
                for (const auto& texture : source.textures)
                {
                    group.textures.push_back(
                        {texture.width, texture.height, texture.mips, texture.layers, texture.format}
                    );
                }
                groups.push_back(std::move(group));
            }
            auto casters = selectShadows(input, settings);
            if (!casters)
            {
                return cxx::unexpected(casters.error());
            }
            const SceneFeatures features{
                dependencies[1],
                dependencies[2],
                dependencies[3],
                dependencies[4],
                dependencies[5],
                dependencies[6],
                dependencies[7]
            };
            auto graph = makeSceneGraph(
                target,
                std::move(groups),
                input.instance_capacity,
                features,
                settings,
                Clustered,
                std::move(*casters)
            );
            if (!graph)
            {
                return cxx::unexpected(graph.error());
            }
            definition = std::move(graph->definition);
            return {};
        }

        RenderResult<void> projectCamera(
            cxx::TypeToken type,
            std::span<const std::byte> source,
            std::span<std::byte> destination
        ) noexcept
        {
            const bool invalid_type = type != cxx::typeToken<CameraInput>();
            const bool invalid_size =
                source.size() != sizeof(CameraInput) || destination.size() != sizeof(LitSceneParameters);
            if (invalid_type || invalid_size)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            CameraInput input;
            std::memcpy(&input, source.data(), sizeof(input));
            LitSceneParameters output;
            output.mesh.view = input.view;
            output.mesh.projection = input.projection;
            output.lighting.inverse_view_projection = input.inverse_view_projection;
            output.lighting.camera_position = {input.position[0], input.position[1], input.position[2], 1};
            output.lighting.viewport = input.viewport;
            output.lighting.background = input.background;
            output.tone.exposure = input.exposure;
            output.shadow.inverse_view_projection = input.inverse_view_projection;
            output.shadow
                .position_near = {input.position[0], input.position[1], input.position[2], input.near_distance};
            output.shadow
                .direction_far = {input.direction[0], input.direction[1], input.direction[2], input.far_distance};
            output.clusters.inverse_view_projection = output.shadow.inverse_view_projection;
            output.clusters.position_near = output.shadow.position_near;
            output.clusters.direction_far = output.shadow.direction_far;
            std::memcpy(destination.data(), &output, sizeof(output));
            return {};
        }

        template <bool Clustered> const FeatureGraphOps& sceneGraph() noexcept
        {
            static constexpr std::array dependencies{
                GBufferFeature::name,
                DeferredLightingFeature::name,
                TonemapFeature::name,
                MeshVisibilityFeature::name,
                ShadowMapFeature::name,
                MeshShadowFeature::name,
                ClusteredLightingFeature::name
            };
            static const std::array inputs{
                CapabilityStorageInput{kIndexedMeshInput, 1, ECapabilityInputScope::GLOBAL, kMeshStorageType, "assets"},
                CapabilityStorageInput{
                    kSampledTextureInput,
                    1,
                    ECapabilityInputScope::GLOBAL,
                    kTextureStorageType,
                    "assets"
                },
                CapabilityStorageInput{
                    kRasterMaterialInput,
                    1,
                    ECapabilityInputScope::GLOBAL,
                    kMaterialStorageType,
                    "assets"
                },
                CapabilityStorageInput{
                    kRasterInstancesInput,
                    1,
                    ECapabilityInputScope::SCENE,
                    kMeshInstanceStorageType,
                    "instances"
                },
                CapabilityStorageInput{
                    kDirectLightsInput,
                    1,
                    ECapabilityInputScope::SCENE,
                    kDirectLightingStorageType,
                    "direct_lights"
                }
            };
            static const FeatureGraphOps graph{
                .expand = expandScene<Clustered>,
                .dependencies = dependencies,
                .effect_input_type = cxx::typeToken<LitSceneInputs>(),
                .parameters = projectCamera,
                .storage_inputs = inputs,
                .decode_configuration = decodeConfiguration
            };
            return graph;
        }

        template <class T>
        RenderPayloadContract payload(std::string_view schema, ERenderPayloadUse use, std::string_view description)
        {
            RenderPayloadContract result;
            result.schema = schema;
            result.use = use;
            result.lifetime = ERenderPayloadLifetime::MOVED_VALUE;
            result.changes_graph = use == ERenderPayloadUse::EFFECT_INPUT || use == ERenderPayloadUse::CONFIGURATION;
            result.description = description;
            result.setCppType(cxx::typeToken<T>());
            return result;
        }

        CapabilitySupport sceneSupport(std::string_view name)
        {
            CapabilitySupport result;
            result.implementation = name;
            result.description = "Lit raster scene with GPU visibility, cooked materials and optional basic shadows.";
            result.configuration = payload<LitSceneConfiguration>(
                kLitSceneConfigurationSchema,
                ERenderPayloadUse::CONFIGURATION,
                "Shadow and light-list configuration."
            );
            result.default_configuration = encodeLitSceneConfiguration(LitSceneConfiguration{})->bytes;
            RenderCapability capability;
            capability.name = kLitSceneCapability;
            capability.description = result.description;
            capability.inputs = RasterAssets::capabilities().capabilities.front().inputs;
            capability.inputs.push_back(payload<LitSceneInputs>(
                kSceneGeometryInput,
                ERenderPayloadUse::EFFECT_INPUT,
                "Cold geometry and light descriptions."
            ));
            capability.inputs.push_back(
                payload<CameraInput>(kCameraInput, ERenderPayloadUse::FRAME_INPUT, "Per-View camera in world space.")
            );
            capability.inputs.push_back(payload<MeshInstancesCreate>(
                kRasterInstancesInput,
                ERenderPayloadUse::RESOURCE_INPUT,
                "Scene instance data; borrowed arrays through Response."
            ));
            capability.inputs.push_back(payload<CreateDirectLights::Request>(
                kDirectLightsInput,
                ERenderPayloadUse::RESOURCE_INPUT,
                "Scene direct lights; borrowed arrays through Response."
            ));
            capability.inputs[capability.inputs.size() - 1].lifetime = ERenderPayloadLifetime::UNTIL_RESPONSE;
            capability.inputs[capability.inputs.size() - 2].lifetime = ERenderPayloadLifetime::UNTIL_RESPONSE;
            capability.outputs = {"color"};
            capability.target_formats = {
                rdesc::ETextureFormat::RGBA8_UNORM,
                rdesc::ETextureFormat::RGBA8_SRGB,
                rdesc::ETextureFormat::BGRA8_UNORM,
                rdesc::ETextureFormat::BGRA8_SRGB
            };
            capability.target_samples = {1};
            capability.target_kinds = {ETargetKind::OFFSCREEN, ETargetKind::SURFACE};
            result.capabilities.push_back(std::move(capability));
            return result;
        }
    } // namespace

    const FeatureGraphOps& ClusteredScene::graph(bool) noexcept
    {
        return sceneGraph<true>();
    }

    const CapabilitySupport& ClusteredScene::capabilities() noexcept
    {
        static const auto support = sceneSupport(name.name());
        return support;
    }

    const FeatureGraphOps& DeferredScene::graph(bool) noexcept
    {
        return sceneGraph<false>();
    }

    const CapabilitySupport& DeferredScene::capabilities() noexcept
    {
        static const auto support = sceneSupport(name.name());
        return support;
    }
} // namespace lux::render::detail

namespace lux::render
{
    RenderResult<LitSceneConfiguration> decodeLitSceneConfiguration(std::span<const std::byte> bytes) noexcept
    {
        LitSceneConfiguration value;
        serialization::BinaryReader reader(bytes);
        auto fields = detail::configurationFields(value);
        const auto decoded = serialization::read(reader, fields, serialization::kPortableMetadataBudget);
        const bool invalid = !decoded || reader.remaining() != 0 || !detail::validConfiguration(value);
        if (invalid)
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        return value;
    }

    RenderResult<SerializedRenderConfiguration> encodeLitSceneConfiguration(const LitSceneConfiguration& value) noexcept
    {
        if (!detail::validConfiguration(value))
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        SerializedRenderConfiguration result{std::string(kLitSceneConfigurationSchema)};
        serialization::BinaryWriter writer(result.bytes);
        const auto encoded =
            serialization::write(writer, detail::configurationFields(value), serialization::kPortableMetadataBudget);
        if (!encoded)
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        return result;
    }

    RenderResult<std::size_t> ConfigValueTraits<LitSceneInputs>::retainedBytes(const LitSceneInputs& value) noexcept
    {
        std::size_t total = sizeof(value);
        const auto add = [&](std::size_t bytes) noexcept
        {
            const auto sum = cxx::checkedAdd(total, bytes);
            if (!sum)
            {
                return false;
            }
            total = *sum;
            return true;
        };
        const auto geometry = cxx::checkedMul(value.geometry.capacity(), sizeof(RasterGeometryInput));
        const auto lights = cxx::checkedMul(value.lights.capacity(), sizeof(SceneLightInput));
        if (!geometry || !lights || !add(*geometry) || !add(*lights))
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        for (const auto& input : value.geometry)
        {
            const auto mesh = ConfigValueTraits<MeshDescription>::retainedBytes(input.geometry);
            const auto program = ConfigValueTraits<rdesc::MaterialProgramSet>::retainedBytes(input.program);
            const auto textures = cxx::checkedMul(input.textures.capacity(), sizeof(ImageShape));
            if (!mesh || !program || !textures || !add(*mesh - sizeof(input.geometry)) ||
                !add(*program - sizeof(input.program)) || !add(*textures))
            {
                return cxx::unexpected(RenderError{kInvalidConfig});
            }
        }
        return total;
    }
} // namespace lux::render
