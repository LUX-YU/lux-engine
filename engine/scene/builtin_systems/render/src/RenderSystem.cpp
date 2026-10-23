#include <lux/engine/render/features/MeshShadowFeature.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/SceneCamera.hpp>
#include <lux/engine/scene/SceneEffect.hpp>

#include <exec/task.hpp>
#include <exec/variant_sender.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/features/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/FeatureGraphOps.hpp>
#include <lux/engine/render/features/GBufferFeature.hpp>
#include <lux/engine/render/features/MeshVisibilityFeature.hpp>
#include <lux/engine/render/features/TonemapFeature.hpp>
#include <lux/engine/render/storage/DirectLighting.hpp>
#include <lux/engine/rendering/RenderAssets.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>
#include <lux/engine/rendering/RenderRequester.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

#include <bit>
#include <cstring>
#include <entt/container/dense_set.hpp>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace lux::scene
{
    using namespace render;
    using namespace rendering;
    using namespace simulation::ecs;
    using detail::SceneParameters;

    namespace
    {
        bool sameTargetRecipe(const TargetDesc& left, const TargetDesc& right) noexcept
        {
            // Built-in Scene recipes declare target-relative extents; dimensions belong to ResizeTarget.
            return left.kind == right.kind && left.format == right.format && left.samples == right.samples &&
                   left.sampled == right.sampled;
        }

        std::vector<std::byte> parameterBytes(const SceneParameters& value)
        {
            const auto bytes = std::as_bytes(std::span(&value, 1));
            return {bytes.begin(), bytes.end()};
        }

        // Cold lifecycle commands only. Ordinary Transform publication uses a previously bound ID.
        template <RenderCommand C> auto request(RenderRequester& client, typename C::Request& input) noexcept
        {
            using VRequest = exec::variant_sender<
                typename RenderRequester::RequestSender<C>,
                decltype(stdexec::just(typename C::Response{}))>;
            auto command = client.bind<C>();
            if (!command)
            {
                typename C::Response result{};
                result.error = command.error();
                return VRequest{stdexec::just(std::move(result))};
            }
            return VRequest{client.request(*command, input)};
        }

        std::uint64_t programHash(const rdesc::MaterialProgramSet& program) noexcept
        {
            std::uint64_t hash = 14695981039346656037ULL;
            for (const auto& use : program.programs)
            {
                for (const auto& stage : use.shader.stages)
                {
                    for (const auto word : stage.words)
                    {
                        hash = (hash ^ word) * 1099511628211ULL;
                    }
                }
            }
            return hash;
        }

        struct MaterialEntry final
        {
            MaterialId material;
            std::vector<RenderAssetVersion> textures;
            std::size_t references = 0;
            std::uint64_t program_hash = 0;
            asset::AssetVfsView snapshot;
            RenderError error;
        };

        struct MeshBinding final
        {
            rdesc::MeshVisualDescription visual;
            std::uint64_t revision = 0;
        };

        struct MeshEntry final
        {
            MeshBinding binding;
            // Candidate uploads never invalidate the currently drawn binding. Previous is retained
            // only until every observation has stopped referring to its compiled resource groups.
            std::optional<MeshBinding> replacement, previous;
            MeshInstanceId instance;
            RenderError error;
            bool present = true;
        };

        struct InstanceInput final
        {
            std::uint64_t entity;
            MeshInstanceDescription description;
        };

        struct LightEntry final
        {
            LightId light;
            rdesc::ELightType type = rdesc::ELightType::POINT;
            bool present = true;
        };

        struct LightInput final
        {
            std::uint64_t entity;
            VDirectLight value;
        };

        RenderResult<VDirectLight> lightValue(
            const rdesc::LightDescription& light,
            const Eigen::Affine3d& world
        ) noexcept
        {
            const auto position = world.translation().cast<float>().eval();
            const auto direction = (-world.linear().col(2)).normalized().cast<float>().eval();
            const std::array<float, 4> radiance{
                light.color[0] * light.intensity,
                light.color[1] * light.intensity,
                light.color[2] * light.intensity,
                0
            };
            switch (light.type)
            {
            case rdesc::ELightType::DIRECTIONAL:
                return VDirectLight{DirectionalLightGpu{radiance, {direction.x(), direction.y(), direction.z(), 0}}};
            case rdesc::ELightType::POINT:
                return VDirectLight{PointLightGpu{
                    {position.x(), position.y(), position.z(), light.range},
                    radiance,
                    {light.attenuation_constant, light.attenuation_linear, light.attenuation_quadratic, 0}
                }};
            case rdesc::ELightType::SPOT:
                return VDirectLight{SpotLightGpu{
                    {position.x(), position.y(), position.z(), light.range},
                    radiance,
                    {direction.x(), direction.y(), direction.z(), std::cos(light.outer_cone_angle)},
                    {light.attenuation_constant,
                     light.attenuation_linear,
                     light.attenuation_quadratic,
                     std::cos(light.inner_cone_angle)}
                }};
            default:
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
        }

        SceneStageResult failure(RenderError error) noexcept
        {
            return cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, error});
        }
    } // namespace

    struct RenderSystem::Impl final
    {
        exec::task<RenderResult<void>> initialize() noexcept
        {
            EmptyRequest empty;
            auto created = co_await request<CreateScene>(client, empty);
            if (created.error.type != 0)
            {
                co_return cxx::unexpected(created.error);
            }
            scene = created.scene;
            const auto storages = co_await request<ListStorages>(client, empty);
            if (storages.error.type != 0)
            {
                co_return cxx::unexpected(storages.error);
            }
            for (const auto& storage : storages.storages)
            {
                const bool instances = storage.name == kMeshInstanceStorageType.name();
                const bool materials = storage.name == kMaterialStorageType.name();
                const bool lights = storage.name == kDirectLightingStorageType.name();
                if (!instances && !materials && !lights)
                {
                    continue;
                }
                PrepareStorageRequest input{
                    {materials ? VStorageLocation{GlobalStorageLocation{}} : VStorageLocation{scene},
                     storage.id,
                     instances   ? "instances"
                     : materials ? "assets"
                                 : "direct_lights"},
                    {}
                };
                auto prepared = co_await request<PrepareStorage>(client, input);
                if (prepared.error.type != 0)
                {
                    co_return cxx::unexpected(prepared.error);
                }
                (instances ? instance_storage : materials ? material_storage : light_storage) = prepared.storage;
            }
            const auto registered_features = co_await request<ListFeatures>(client, empty);
            if (registered_features.error.type != 0)
            {
                co_return cxx::unexpected(registered_features.error);
            }
            for (const auto& feature : registered_features.features)
            {
                if (feature.name == GBufferFeatureDescriptor::name.name())
                {
                    features.gbuffer = feature.id;
                }
                if (feature.name == DeferredLightingFeatureDescriptor::name.name())
                {
                    features.lighting = feature.id;
                }
                if (feature.name == TonemapFeatureDescriptor::name.name())
                {
                    features.tonemap = feature.id;
                }
                if (feature.name == MeshVisibilityFeatureDescriptor::name.name())
                {
                    features.visibility = feature.id;
                }
            }
            for (const auto& feature : registered_features.features)
            {
                if (feature.name == ShadowMapFeatureDescriptor::name.name())
                {
                    features.shadows = feature.id;
                }
                if (feature.name == MeshShadowFeatureDescriptor::name.name())
                {
                    features.mesh_shadow = feature.id;
                }
                if (feature.name == ClusteredLightingFeatureDescriptor::name.name())
                {
                    features.clusters = feature.id;
                }
            }
            const bool missing_storage =
                instance_storage.isNull() || material_storage.isNull() || light_storage.isNull();
            const bool missing_feature =
                features.gbuffer.isNull() || features.lighting.isNull() || features.tonemap.isNull() ||
                features.visibility.isNull() ||
                (configuration.shadows && (features.shadows.isNull() || features.mesh_shadow.isNull())) ||
                (configuration.recipe == "lux.scene.clustered" && features.clusters.isNull());
            if (missing_storage || missing_feature)
            {
                co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
            initialized = true;
            co_return RenderResult<void>{};
        }

        RenderAssetVersion assetKey(asset::AssetId id, std::uint64_t revision) const noexcept
        {
            return {id, revision, asset_source};
        }

        void releaseBinding(const MeshBinding& binding) noexcept
        {
            assets.releaseMesh(assetKey(binding.visual.mesh, binding.revision));
            --materials.find(assetKey(binding.visual.material, binding.revision))->second.references;
        }

        RenderResult<void> acquireBinding(const MeshBinding& binding) noexcept
        {
            const auto mesh_key = assetKey(binding.visual.mesh, binding.revision);
            const auto material_key = assetKey(binding.visual.material, binding.revision);
            auto acquired_mesh = assets.acquireMesh(mesh_key, source, view_inputs.asset_snapshot);
            if (!acquired_mesh)
            {
                return acquired_mesh;
            }
            auto material = materials.find(material_key);
            if (material == materials.end())
            {
                auto acquired = assets.acquireMaterial(material_key, source, view_inputs.asset_snapshot);
                if (!acquired)
                {
                    assets.releaseMesh(mesh_key);
                    return acquired;
                }
                material = materials.emplace(material_key, MaterialEntry{.snapshot = view_inputs.asset_snapshot}).first;
                pending_materials.insert(material_key);
            }
            ++material->second.references;
            return {};
        }

        bool bindingReady(const MeshBinding& binding, RenderError& failure) const noexcept
        {
            const auto key = assetKey(binding.visual.mesh, binding.revision);
            const auto& material = materials.find(assetKey(binding.visual.material, binding.revision))->second;
            failure = assets.failure(key);
            if (failure.type == 0)
            {
                failure = material.error;
            }
            return failure.type == 0 && assets.mesh(key) && !material.material.isNull();
        }

        exec::task<void> prepareMaterials() noexcept
        {
            for (auto pending = pending_materials.begin(); pending != pending_materials.end();)
            {
                const auto key = *pending++;
                auto& entry = materials.find(key)->second;
                const auto reject = [&](RenderError failure) noexcept
                {
                    entry.error = failure;
                    entry.snapshot = {};
                    pending_materials.erase(key);
                    reportRenderFailure(failure, "Prepare Scene material candidate");
                };
                if (entry.references == 0 || !entry.material.isNull())
                {
                    continue;
                }
                const auto* cooked = assets.material(key);
                if (!cooked)
                {
                    const auto error = assets.failure(key);
                    if (error.type != 0)
                    {
                        reject(error);
                    }
                    continue;
                }
                // Incremental admission can stop at the actual Asset limit without reacquiring earlier slots.
                for (std::size_t i = entry.textures.size(); i < cooked->texture_slot_ids.size(); ++i)
                {
                    const RenderAssetVersion texture{cooked->texture_slot_ids[i], key.revision, key.source};
                    if (!texture.asset.isNull())
                    {
                        auto acquired = assets.acquireTexture(texture, source, entry.snapshot);
                        if (!acquired && acquired.error().type == kRuntimeCapacity)
                        {
                            break;
                        }
                        if (!acquired)
                        {
                            reject(acquired.error());
                            break;
                        }
                    }
                    entry.textures.push_back(texture);
                }
                if (entry.textures.size() != cooked->texture_slot_ids.size())
                {
                    continue;
                }
                MaterialWrite input{material_storage, cooked->programs, cooked->programs.parameter_defaults};
                bool ready = true;
                for (auto texture : entry.textures)
                {
                    if (texture.asset.isNull())
                    {
                        input.textures.push_back({});
                        continue;
                    }
                    const auto* uploaded = assets.texture(texture);
                    if (!uploaded)
                    {
                        const auto error = assets.failure(texture);
                        if (error.type != 0)
                        {
                            reject(error);
                        }
                        ready = false;
                        break;
                    }
                    input.textures.push_back(uploaded->texture);
                }
                if (!ready)
                {
                    continue;
                }
                auto created = co_await request<CreateMaterial>(client, input);
                if (created.error.type != 0)
                {
                    reject(created.error);
                    continue;
                }
                entry.material = created.material;
                entry.program_hash = programHash(cooked->programs);
                entry.snapshot = {};
                pending_materials.erase(key);
            }
            co_return;
        }

        std::vector<RasterMeshGroup> prepareDrawGroups(bool include_replacements) noexcept
        {
            GBufferFeatureConfig surface;
            surface.visible_capacity =
                std::bit_ceil(std::max<std::uint32_t>(128, static_cast<std::uint32_t>(meshes.size())));
            std::unordered_multimap<std::uint64_t, std::size_t> group_indices;
            std::vector<const rdesc::MaterialProgramSet*> group_programs;
            std::vector<const MeshBinding*> bindings;
            for (const auto& [entity, entry] : meshes)
            {
                if (!entry.present || entry.instance.isNull())
                {
                    continue;
                }
                bindings.push_back(&entry.binding);
                RenderError failure;
                if (include_replacements && entry.replacement && bindingReady(*entry.replacement, failure))
                {
                    bindings.push_back(&*entry.replacement);
                }
            }
            for (const auto* binding : bindings)
            {
                const auto& mesh = *assets.mesh(assetKey(binding->visual.mesh, binding->revision));
                const auto& material = materials.find(assetKey(binding->visual.material, binding->revision))->second;
                const auto& program = assets.material(assetKey(binding->visual.material, binding->revision))->programs;
                const auto bucket = material.program_hash ^ (std::uint64_t(mesh.mesh.local.index) << 32) ^
                                    mesh.mesh.local.gen ^ mesh.mesh.storage.index;
                const auto [first, last] = group_indices.equal_range(bucket);
                auto found = first;
                for (; found != last; ++found)
                {
                    const auto& group = surface.groups[found->second];
                    if (group.group.mesh == mesh.mesh &&
                        (group_programs[found->second] == &program || *group_programs[found->second] == program))
                    {
                        break;
                    }
                }
                if (found != last)
                {
                    auto& group = surface.groups[found->second];
                    const MeshInstanceDrawGroup candidate{mesh.mesh, material.material};
                    if (!(candidate < group.group))
                    {
                        continue;
                    }
                    // Deterministic table representative; instance iteration order is not a recipe change.
                    group.group = candidate;
                    group.textures.clear();
                    for (const auto texture : material.textures)
                    {
                        const auto* value = texture.asset.isNull() ? nullptr : assets.texture(texture);
                        group.textures.push_back(
                            value ? value->description : GraphImageDesc{1, 1, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM}
                        );
                    }
                    continue;
                }
                group_indices.emplace(bucket, surface.groups.size());
                group_programs.push_back(&program);
                RasterMeshGroup group{mesh.geometry, program, {mesh.mesh, material.material}};
                group.mesh_storage = "assets";
                group.material_storage = "assets";
                // Keep the raster recipe's canonical winding. Camera projection is not an additional
                // source-mesh winding conversion; mirrored instance transforms are handled by the Mesh Feature.
                for (const auto texture : material.textures)
                {
                    const auto* value = texture.asset.isNull() ? nullptr : assets.texture(texture);
                    group.textures.push_back(
                        value ? value->description : GraphImageDesc{1, 1, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM}
                    );
                }
                surface.groups.push_back(std::move(group));
            }
            std::ranges::sort(
                surface.groups,
                [](const auto& left, const auto& right) noexcept { return left.group < right.group; }
            );
            return std::move(surface.groups);
        }

        struct Observation final
        {
            ViewId view;
            EffectId effect, effect_to_release;
            AttachmentId attachment;
            SceneViewRequest adopted;
            SceneParameters parameters;
            RenderError error;
            std::vector<MeshInstanceDrawGroup> groups;
            std::vector<ShadowLightSelection> casters;
            std::uint64_t revision = 0;
            std::uint32_t capacity = 0;
            bool parameters_changed = true;
        };

        RenderResult<std::vector<ShadowLightSelection>> shadowCasters(const SceneParameters& parameters) const noexcept
        {
            std::vector<ShadowLightSelection> result;
            if (!configuration.shadows)
            {
                return result;
            }
            const float near_distance = parameters.shadow.position_near[3];
            const float far_distance = std::min(configuration.shadow_distance, parameters.shadow.direction_far[3]);
            for (const auto& [bits, entry] : lights)
            {
                const auto* source = registry.try_get<const Light3D>(static_cast<Entity>(bits));
                if (!entry.present || entry.light.isNull() || !source || !source->value.cast_shadow)
                {
                    continue;
                }
                const auto& light = source->value;
                const bool invalid_count =
                    light.cascade_count == 0 || light.cascade_count > light.cascade_splits.size();
                if (entry.type == rdesc::ELightType::DIRECTIONAL && (invalid_count || far_distance <= near_distance))
                {
                    return cxx::unexpected(RenderError{kCommandContract});
                }
                ShadowLightSelection selected;
                selected.light = entry.light;
                selected.kind = entry.type == rdesc::ELightType::DIRECTIONAL ? EDirectLightingBuffer::DIRECTIONAL
                                : entry.type == rdesc::ELightType::POINT     ? EDirectLightingBuffer::POINT
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
            std::ranges::sort(
                result,
                [](const auto& left, const auto& right) noexcept
                {
                    return std::tie(left.light.local.index, left.light.local.gen) <
                           std::tie(right.light.local.index, right.light.local.gen);
                }
            );
            return result;
        }

        exec::task<RenderResult<void>> releaseEffects(Observation& observation) noexcept
        {
            for (auto* handle : {&observation.effect_to_release, &observation.effect})
            {
                if (handle->isNull())
                {
                    continue;
                }
                EffectRequest remove{*handle};
                const auto result = co_await request<RemoveEffect>(client, remove);
                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
                *handle = {};
            }
            co_return RenderResult<void>{};
        }

        exec::task<RenderResult<void>> withdrawView(Observation& observation) noexcept
        {
            // Stop future parameter publications before enqueuing removal on the same Main link.
            observation.attachment = {};
            if (!observation.view.isNull())
            {
                ViewRequest remove{scene, observation.view};
                const auto result = co_await request<RemoveView>(client, remove);
                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
                observation.view = {};
            }
            co_return co_await releaseEffects(observation);
        }

        exec::task<RenderResult<void>> synchronizeViews(
            bool include_replacements = false,
            bool* groups_adopted = nullptr
        ) noexcept
        {
            const auto accepted_topology = topology_revision;
            auto groups = prepareDrawGroups(include_replacements);
            std::vector<MeshInstanceDrawGroup> selection;
            selection.reserve(groups.size());
            for (const auto& group : groups)
            {
                selection.push_back(group.group);
            }
            const auto capacity =
                std::bit_ceil(std::max<std::uint32_t>(128, static_cast<std::uint32_t>(meshes.size())));
            // Only this structural coroutine changes the map; references remain stable across suspension.
            for (auto entry = observations.begin(); entry != observations.end();)
            {
                if (view_inputs.findView(entry->first))
                {
                    ++entry;
                    continue;
                }
                auto withdrawn = co_await withdrawView(entry->second);
                if (!withdrawn)
                {
                    co_return withdrawn;
                }
                entry = observations.erase(entry);
            }
            std::vector<SceneViewInput> accepted(view_inputs.views().begin(), view_inputs.views().end());
            for (const auto& input : accepted)
            {
                // Input storage may move or disappear while another View compiles.
                const auto* current = view_inputs.findView(input.id);
                if (!current || current->revision != input.revision)
                {
                    continue;
                }
                auto& observation = observations[input.id];
                auto parameters = detail::sceneParameters(registry, input.request);
                if (!parameters)
                {
                    observation.error = parameters.error();
                    auto withdrawn = co_await withdrawView(observation);
                    if (!withdrawn)
                    {
                        co_return withdrawn;
                    }
                    continue;
                }
                observation.error = {};
                auto selected_casters = shadowCasters(*parameters);
                if (!selected_casters)
                {
                    observation.error = selected_casters.error();
                    continue;
                }
                auto casters = std::move(*selected_casters);
                const bool has_same_recipe =
                    !observation.attachment.isNull() && observation.adopted.target == input.request.target &&
                    sameTargetRecipe(observation.adopted.description, input.request.description) &&
                    observation.capacity >= capacity && observation.groups == selection &&
                    observation.casters == casters;
                if (has_same_recipe)
                {
                    observation.parameters_changed |= !detail::sameParameters(observation.parameters, *parameters) ||
                                                      observation.adopted.viewport != input.request.viewport;
                    observation.parameters = *parameters;
                    observation.adopted = input.request;
                    observation.revision = input.revision;
                    continue;
                }
                if (observation.view.isNull())
                {
                    CreateViewRequest create{scene};
                    const auto result = co_await request<CreateView>(client, create);
                    if (result.error.type != 0)
                    {
                        co_return cxx::unexpected(result.error);
                    }
                    observation.view = result.view;
                }
                auto recipe = detail::makeSceneEffect(
                    input.request.description,
                    groups,
                    capacity,
                    features,
                    configuration,
                    casters
                );
                if (!recipe)
                {
                    co_return cxx::unexpected(recipe.error());
                }
                const auto compiled = co_await request<CreateEffect>(client, *recipe);
                if (compiled.error.type != 0)
                {
                    co_return cxx::unexpected(compiled.error);
                }
                observation.effect_to_release = compiled.effect;
                current = view_inputs.findView(input.id);
                auto latest_parameters = detail::sceneParameters(registry, input.request);
                const bool is_stale_input = !current || current->revision != input.revision || !latest_parameters ||
                                            accepted_topology != topology_revision;
                if (!is_stale_input)
                {
                    AttachmentRequest attach{
                        scene,
                        observation.view,
                        input.request.target,
                        compiled.effect,
                        parameterBytes(*latest_parameters),
                        input.request.viewport,
                        {},
                        observation.attachment
                    };
                    const auto adopted = co_await request<Attach>(client, attach);
                    if (adopted.error.type != 0)
                    {
                        co_return cxx::unexpected(adopted.error);
                    }
                    std::swap(observation.effect, observation.effect_to_release);
                    observation.attachment = adopted.attachment;
                    observation.adopted = input.request;
                    observation.parameters = *latest_parameters;
                    observation.parameters_changed = true;
                    observation.revision = input.revision;
                    observation.capacity = capacity;
                    observation.groups = selection;
                    observation.casters = std::move(casters);
                }
                else
                {
                    view_revision = UINT64_MAX;
                }
                if (!observation.effect_to_release.isNull())
                {
                    EffectRequest remove{observation.effect_to_release};
                    const auto removed = co_await request<RemoveEffect>(client, remove);
                    if (removed.error.type != 0)
                    {
                        co_return cxx::unexpected(removed.error);
                    }
                    observation.effect_to_release = {};
                }
            }
            if (groups_adopted)
            {
                *groups_adopted = std::ranges::all_of(
                    observations,
                    [&](const auto& entry) noexcept
                    { return entry.second.attachment.isNull() || entry.second.groups == selection; }
                );
            }
            co_return RenderResult<void>{};
        }

        exec::task<RenderResult<void>> synchronizeStructure(
            std::vector<InstanceInput> inputs,
            std::vector<LightInput> light_inputs
        ) noexcept
        {
            if (!initialized)
            {
                auto result = co_await initialize();
                if (!result)
                {
                    co_return result;
                }
            }
            if (closing)
            {
                co_return RenderResult<void>{};
            }
            bool changed = rebuild_requested;
            bool groups_pruned = false;
            for (auto& [entity, entry] : meshes)
            {
                if (entry.present || entry.instance.isNull())
                {
                    continue;
                }
                auto removed = co_await request<ReleaseMeshInstance>(client, entry.instance);
                if (removed.error.type != 0)
                {
                    co_return cxx::unexpected(removed.error);
                }
                entry.instance = {};
                changed = true;
            }
            for (auto entry = lights.begin(); entry != lights.end();)
            {
                if (entry->second.present)
                {
                    ++entry;
                    continue;
                }
                if (!entry->second.light.isNull())
                {
                    RemoveDirectLight::Request remove{entry->second.light};
                    auto removed = co_await request<RemoveDirectLight>(client, remove);
                    if (removed.error.type != 0)
                    {
                        co_return cxx::unexpected(removed.error);
                    }
                }
                entry = lights.erase(entry);
                changed = true;
            }
            if (!light_inputs.empty())
            {
                changed = true;
                std::vector<VDirectLight> values;
                values.reserve(light_inputs.size());
                for (const auto& input : light_inputs)
                {
                    values.push_back(input.value);
                }
                CreateDirectLights::Request create{light_storage, 0, values};
                auto result = co_await request<CreateDirectLights>(client, create);
                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
                for (std::size_t i = 0; i < light_inputs.size(); ++i)
                {
                    lights.find(light_inputs[i].entity)->second.light = result.lights[i];
                    new_lights.push_back(light_inputs[i].entity);
                }
            }
            co_await prepareMaterials();
            std::vector<MeshInstanceDescription> descriptions;
            std::vector<std::uint64_t> entities;
            std::vector<MeshInstanceUpdate> replacements;
            std::vector<std::uint64_t> replacement_entities;
            for (auto& input : inputs)
            {
                auto& entry = meshes.find(input.entity)->second;
                if (!entry.present)
                {
                    continue;
                }
                const auto& binding = entry.replacement ? *entry.replacement : entry.binding;
                if (!bindingReady(binding, entry.error))
                {
                    if (entry.error.type != 0)
                    {
                        // Failed candidates remain diagnosable; a new author/asset revision retries them.
                        pending_instances.erase(input.entity);
                    }
                    continue;
                }
                const auto* mesh = assets.mesh(assetKey(binding.visual.mesh, binding.revision));
                const auto material = materials.find(assetKey(binding.visual.material, binding.revision));
                input.description.mesh = mesh->mesh;
                input.description.material = material->second.material;
                if (entry.instance.isNull())
                {
                    descriptions.push_back(input.description);
                    entities.push_back(input.entity);
                }
                else if (entry.replacement)
                {
                    replacements.push_back({entry.instance, input.description});
                    replacement_entities.push_back(input.entity);
                }
            }
            if (!descriptions.empty())
            {
                MeshInstancesCreate create{instance_storage, descriptions};
                auto created = co_await request<CreateMeshInstances>(client, create);
                if (created.error.type != 0)
                {
                    for (const auto entity : entities)
                    {
                        meshes.find(entity)->second.error = created.error;
                        pending_instances.erase(entity);
                    }
                    reportRenderFailure(created.error, "Create Scene instance candidates");
                }
                else
                {
                    for (std::size_t i = 0; i < entities.size(); ++i)
                    {
                        auto& entry = meshes.find(entities[i])->second;
                        entry.instance = created.instances[i];
                        if (entry.replacement)
                        {
                            releaseBinding(entry.binding);
                            entry.binding = std::move(*entry.replacement);
                            entry.replacement.reset();
                        }
                        new_instances.push_back(entities[i]);
                        pending_instances.erase(entities[i]);
                    }
                }
                changed = true;
            }
            if ((changed || !replacements.empty()) && !closing)
            {
                const auto accepted_views = view_inputs.revision();
                const auto accepted_topology = topology_revision;
                // A temporary recipe accepts both old and candidate groups. It changes no instance:
                // every observation can keep drawing the old binding until the single frame update.
                bool groups_adopted = false;
                auto rebuilt = co_await synchronizeViews(!replacements.empty(), &groups_adopted);
                if (!rebuilt)
                {
                    co_return rebuilt;
                }
                const bool changed_during_compile = !groups_adopted || accepted_views != view_inputs.revision() ||
                                                    accepted_topology != topology_revision;
                rebuild_requested = changed_during_compile;
                groups_pruned = replacements.empty() && groups_adopted;
                if (!replacements.empty() && !changed_during_compile)
                {
                    MeshInstanceBatch update{instance_storage, ++sequence, replacements};
                    const auto result = co_await request<UpdateMeshInstances>(client, update);
                    if (result.error.type == 0)
                    {
                        for (const auto entity : replacement_entities)
                        {
                            auto& entry = meshes.find(entity)->second;
                            entry.previous = std::move(entry.binding);
                            entry.binding = std::move(*entry.replacement);
                            entry.replacement.reset();
                            pending_instances.erase(entity);
                            new_instances.push_back(entity);
                        }
                        auto pruned = co_await synchronizeViews(false, &groups_pruned);
                        if (!pruned)
                        {
                            co_return pruned; // Previous resource references still belong to MeshEntry.
                        }
                    }
                    else
                    {
                        for (const auto entity : replacement_entities)
                        {
                            meshes.find(entity)->second.error = result.error;
                            pending_instances.erase(entity);
                        }
                        reportRenderFailure(result.error, "Adopt Scene instance candidates");
                    }
                }
            }
            rebuild_requested |= std::ranges::any_of(
                                     meshes,
                                     [](const auto& value) noexcept { return value.second.previous.has_value(); }
                                 ) &&
                                 !groups_pruned;
            // All current recipes have stopped mentioning these resources. Native retirement still
            // governs actual GPU use; this only releases the Engine's asset reference.
            for (auto& [entity, entry] : meshes)
            {
                if (entry.previous && groups_pruned)
                {
                    releaseBinding(*entry.previous);
                    entry.previous.reset();
                }
            }
            for (auto entry = meshes.begin(); entry != meshes.end();)
            {
                if (entry->second.present || !groups_pruned)
                {
                    ++entry;
                    continue;
                }
                releaseBinding(entry->second.binding);
                if (entry->second.replacement)
                {
                    releaseBinding(*entry->second.replacement);
                }
                pending_instances.erase(entry->first);
                entry = meshes.erase(entry);
            }
            for (auto entry = materials.begin(); entry != materials.end();)
            {
                if (entry->second.references != 0)
                {
                    ++entry;
                    continue;
                }
                if (!entry->second.material.isNull())
                {
                    auto removed = co_await request<ReleaseMaterial>(client, entry->second.material);
                    if (removed.error.type != 0)
                    {
                        co_return cxx::unexpected(removed.error);
                    }
                }
                for (const auto texture : entry->second.textures)
                {
                    if (!texture.asset.isNull())
                    {
                        assets.releaseTexture(texture);
                    }
                }
                assets.releaseMaterial(entry->first);
                pending_materials.erase(entry->first);
                entry = materials.erase(entry);
            }
            co_return RenderResult<void>{};
        }

        exec::task<RenderResult<void>> closeScene() noexcept
        {
            if (!scene.isNull())
            {
                SceneRequest remove{scene};
                auto result = co_await request<RemoveScene>(client, remove);
                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
                scene = {};
                for (auto& [id, observation] : observations)
                {
                    observation.view = {};
                    observation.attachment = {};
                }
            }
            for (auto entry = observations.begin(); entry != observations.end();)
            {
                auto released = co_await releaseEffects(entry->second);
                if (!released)
                {
                    co_return released;
                }
                entry = observations.erase(entry);
            }
            while (!meshes.empty())
            {
                const auto entry = meshes.begin();
                releaseBinding(entry->second.binding);
                if (entry->second.replacement)
                {
                    releaseBinding(*entry->second.replacement);
                }
                if (entry->second.previous)
                {
                    releaseBinding(*entry->second.previous);
                }
                meshes.erase(entry);
            }
            while (!materials.empty())
            {
                const auto entry = materials.begin();
                if (!entry->second.material.isNull())
                {
                    auto result = co_await request<ReleaseMaterial>(client, entry->second.material);
                    if (result.error.type != 0)
                    {
                        co_return cxx::unexpected(result.error);
                    }
                }
                for (const auto texture : entry->second.textures)
                {
                    if (!texture.asset.isNull())
                    {
                        assets.releaseTexture(texture);
                    }
                }
                assets.releaseMaterial(entry->first);
                materials.erase(entry);
            }
            pending_instances.clear();
            pending_materials.clear();
            lights.clear();
            co_return RenderResult<void>{};
        }

        void startTransaction(exec::task<RenderResult<void>> work) noexcept
        {
            transaction_pending = true;
            auto ready = execution::taskResult(stdexec::starts_on(execution.mainScheduler(), std::move(work)));
            auto accepted = execution.spawn(stdexec::then(
                std::move(ready),
                [this](auto result) noexcept
                {
                    error = result                           ? RenderError{}
                            : result.error().domainFailure() ? *result.error().domainFailure()
                                                             : RenderError{kRuntimeState};
                    if (error.type != 0)
                    {
                        reportRenderFailure(error, closing ? "Close Render Scene" : "Synchronize Render Scene");
                    }
                    transaction_pending = false;
                }
            ));
            if (!accepted)
            {
                transaction_pending = false;
                error = RenderError{kRuntimeState};
                reportRenderFailure(error, "Admit Render Scene operation");
            }
        }

        RenderResult<bool> collectMeshChanges() noexcept
        {
            bool changed = false;
            processed.clear();
            for (const auto entity : structure)
            {
                const auto bits = entityBits(entity);
                const auto* mesh = registry.valid(entity) ? registry.try_get<const Mesh3D>(entity) : nullptr;
                const bool visible = mesh && mesh->value.visible && registry.all_of<WorldTransform3D>(entity);
                auto current = meshes.find(bits);
                if (!visible)
                {
                    if (current != meshes.end())
                    {
                        current->second.present = false;
                        changed = true;
                    }
                    processed.push_back(entity);
                    continue;
                }
                if (current != meshes.end())
                {
                    auto& entry = current->second;
                    if (!entry.present || entry.previous)
                    {
                        continue; // Finish withdrawing an already accepted binding before another replacement.
                    }
                    const auto& desired = entry.replacement ? *entry.replacement : entry.binding;
                    if (desired.visual == mesh->value && desired.revision == asset_revision)
                    {
                        processed.push_back(entity);
                        continue;
                    }
                }
                MeshBinding candidate{mesh->value, asset_revision};
                auto acquired = acquireBinding(candidate);
                if (!acquired)
                {
                    if (acquired.error().type == kRuntimeCapacity)
                    {
                        continue;
                    }
                    return cxx::unexpected(acquired.error());
                }
                if (current == meshes.end())
                {
                    meshes.emplace(bits, MeshEntry{.binding = std::move(candidate)});
                }
                else
                {
                    auto& entry = current->second;
                    if (entry.replacement)
                    {
                        releaseBinding(*entry.replacement);
                    }
                    entry.replacement = std::move(candidate);
                    entry.error = {};
                }
                pending_instances.insert(bits);
                changed = true;
                processed.push_back(entity);
            }
            for (auto entity : processed)
            {
                structure.erase(entity);
            }
            return changed;
        }

        RenderResult<bool> collectLightChanges(std::vector<LightInput>& light_inputs) noexcept
        {
            bool changed = false;
            for (const auto entity : light_changes)
            {
                const auto bits = entityBits(entity);
                const auto* light = registry.valid(entity) ? registry.try_get<const Light3D>(entity) : nullptr;
                const auto* pose = registry.valid(entity) ? registry.try_get<const WorldTransform3D>(entity) : nullptr;
                auto found = lights.find(bits);
                if (!light || !pose)
                {
                    if (found != lights.end())
                    {
                        found->second.present = false;
                        changed = true;
                    }
                    continue;
                }
                if (found == lights.end())
                {
                    found = lights.emplace(bits, LightEntry{{}, light->value.type}).first;
                }
                if (found->second.type != light->value.type)
                {
                    found->second.present = false;
                    changed = true;
                    continue;
                }
                if (found->second.light.isNull())
                {
                    auto value = lightValue(light->value, pose->value);
                    if (!value)
                    {
                        return cxx::unexpected(value.error());
                    }
                    light_inputs.push_back({bits, std::move(*value)});
                    changed = true;
                }
            }
            return changed;
        }

        bool needsViewSynchronization() const noexcept
        {
            if (observations.size() != view_inputs.views().size())
            {
                return true;
            }
            for (const auto& input : view_inputs.views())
            {
                const auto found = observations.find(input.id);
                if (found == observations.end())
                {
                    return true;
                }
                const auto& observation = found->second;
                const bool is_changed_output =
                    observation.adopted.target != input.request.target ||
                    !sameTargetRecipe(observation.adopted.description, input.request.description);
                if (is_changed_output || observation.attachment.isNull())
                {
                    return true;
                }
                if (configuration.shadows)
                {
                    const auto candidate = detail::sceneParameters(registry, input.request);
                    if (!candidate ||
                        candidate->shadow.position_near[3] != observation.parameters.shadow.position_near[3] ||
                        candidate->shadow.direction_far[3] != observation.parameters.shadow.direction_far[3])
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        void prepareViewParameters() noexcept
        {
            for (auto& [id, observation] : observations)
            {
                const auto* input = view_inputs.findView(id);
                if (!input || observation.attachment.isNull())
                {
                    continue;
                }
                const bool has_new_input = observation.revision != input->revision;
                if (!camera_dirty && !has_new_input)
                {
                    continue;
                }
                // During candidate compilation the active Attachment keeps its own target/aspect inputs.
                const bool same_output = observation.adopted.target == input->request.target &&
                                         sameTargetRecipe(observation.adopted.description, input->request.description);
                const auto& requested = same_output ? input->request : observation.adopted;
                auto candidate = detail::sceneParameters(registry, requested);
                if (!candidate)
                {
                    observation.error = candidate.error();
                    continue;
                }
                observation.error = {};
                observation.parameters_changed |= !detail::sameParameters(observation.parameters, *candidate) ||
                                                  observation.adopted.viewport != requested.viewport;
                observation.parameters = *candidate;
                if (same_output)
                {
                    observation.adopted = requested;
                    observation.revision = input->revision;
                }
            }
            camera_dirty = false;
        }

        Impl(Registry& registry, const RenderSystemDependencies& dependencies, RenderSystemConfiguration selected)
            : registry(registry), view_inputs(dependencies.inputs), client(dependencies.client),
              execution(dependencies.execution), assets(dependencies.assets), source(dependencies.source),
              asset_source(dependencies.inputs.asset_source), configuration(std::move(selected)),
              mesh_added(registry.on_construct<Mesh3D>().connect<&Impl::structureChanged>(*this)),
              mesh_updated(registry.on_update<Mesh3D>().connect<&Impl::structureChanged>(*this)),
              mesh_removed(registry.on_destroy<Mesh3D>().connect<&Impl::structureChanged>(*this)),
              pose_added(registry.on_construct<WorldTransform3D>().connect<&Impl::poseAdded>(*this)),
              pose_updated(registry.on_update<WorldTransform3D>().connect<&Impl::transformChanged>(*this)),
              pose_removed(registry.on_destroy<WorldTransform3D>().connect<&Impl::poseRemoved>(*this)),
              camera_added(registry.on_construct<Camera>().connect<&Impl::cameraChanged>(*this)),
              camera_updated(registry.on_update<Camera>().connect<&Impl::cameraChanged>(*this)),
              camera_removed(registry.on_destroy<Camera>().connect<&Impl::cameraChanged>(*this)),
              light_added(registry.on_construct<Light3D>().connect<&Impl::lightDescriptionChanged>(*this)),
              light_updated(registry.on_update<Light3D>().connect<&Impl::lightDescriptionChanged>(*this)),
              light_removed(registry.on_destroy<Light3D>().connect<&Impl::lightDescriptionChanged>(*this))
        {
            if (!validRenderConfiguration(configuration))
            {
                error = RenderError{kCommandContract};
                return;
            }
            for (const auto entity : registry.view<const Mesh3D>())
            {
                structure.insert(entity);
            }
            for (const auto entity : registry.view<const Light3D>())
            {
                light_changes.insert(entity);
            }
            auto transforms_command = dependencies.client.bind<UpdateTransforms>();
            auto lighting_command = dependencies.client.bind<UpdateDirectLights>();
            auto inputs_command = dependencies.client.bind<UpdateAttachmentInputs>();
            if (!transforms_command || !lighting_command || !inputs_command)
            {
                error = RenderError{kRuntimeUnsupported};
                return;
            }
            transforms = *transforms_command;
            lighting = *lighting_command;
            attachment_inputs = *inputs_command;
        }

        ~Impl() noexcept
        {
            if (transaction_pending || !scene.isNull() || !observations.empty() || !meshes.empty() ||
                !materials.empty())
            {
                std::terminate(); // Retirement maintenance finishes before the System is destroyed.
            }
        }

        void structureChanged(Registry&, Entity entity) noexcept
        {
            if (!structure.contains(entity))
            {
                structure.insert(entity);
            }
        }

        void poseAdded(Registry& registry, Entity entity) noexcept
        {
            ++topology_revision;
            structureChanged(registry, entity);
            transformChanged(registry, entity);
        }

        void cameraChanged(Registry&, Entity) noexcept
        {
            camera_dirty = true;
            ++topology_revision;
        }

        void lightDescriptionChanged(Registry& registry, Entity entity) noexcept
        {
            ++topology_revision;
            lightChanged(registry, entity);
        }

        void poseRemoved(Registry& registry, Entity entity) noexcept
        {
            ++topology_revision;
            structureChanged(registry, entity);
            if (registry.all_of<Camera>(entity))
            {
                camera_dirty = true;
            }
            if (registry.all_of<Light3D>(entity))
            {
                lightChanged(registry, entity);
            }
        }

        void lightChanged(Registry&, Entity entity) noexcept
        {
            if (!light_changes.contains(entity))
            {
                light_changes.insert(entity);
            }
        }

        void transformChanged(Registry&, Entity entity) noexcept
        {
            if (registry.all_of<Light3D>(entity))
            {
                lightChanged(registry, entity);
            }
            if (registry.all_of<Camera>(entity))
            {
                camera_dirty = true;
            }
            if (!transformed.contains(entity))
            {
                transformed.insert(entity);
            }
        }

        Registry& registry;
        SceneRenderInputs& view_inputs;
        RenderRequester& client;
        execution::ExecutionRuntime& execution;
        RenderAssets& assets;
        process::asset_loading::AssetReadPort source;
        const std::uint64_t asset_source;
        std::unordered_map<std::uint64_t, MeshEntry> meshes;
        std::vector<std::uint64_t> new_instances, new_lights;
        std::unordered_set<std::uint64_t> pending_instances;
        std::unordered_map<std::uint64_t, LightEntry> lights;
        std::map<RenderAssetVersion, MaterialEntry> materials;
        std::set<RenderAssetVersion> pending_materials;
        SceneId scene;
        std::unordered_map<SceneViewId, Observation, SceneViewId::Hash> observations;
        StorageId instance_storage, material_storage, light_storage;
        detail::SceneEffectFeatures features;
        RenderSystemConfiguration configuration;
        RenderError error;
        bool initialized = false, transaction_pending = false, closing = false;
        bool rebuild_requested = false;

        // Dirty intents can contain an erased entity and its reused slot's next generation in
        // one Main interval (for example Undo + Redo). A live-component sparse set cannot.
        entt::dense_set<Entity> structure, transformed, light_changes;
        std::vector<Entity> processed;
        std::vector<cxx::SlotKey<MeshInstanceTag>> transform_ids;
        std::vector<Entity> transform_entities;
        std::vector<DirectLightUpdate> light_updates;
        std::optional<BoundCommand<UpdateTransforms>> transforms;
        std::optional<BoundCommand<UpdateDirectLights>> lighting;
        std::optional<BoundCommand<UpdateAttachmentInputs>> attachment_inputs;
        std::uint64_t sequence = 0, observed_assets = 0, resynchronization = 0;
        bool camera_dirty = true;
        std::uint64_t view_revision = UINT64_MAX, asset_revision = 0;
        std::uint64_t topology_revision = 0, observed_topology = UINT64_MAX;
        entt::scoped_connection mesh_added, mesh_updated, mesh_removed, pose_added, pose_updated, pose_removed;
        entt::scoped_connection camera_added, camera_updated, camera_removed;
        entt::scoped_connection light_added, light_updated, light_removed;
    };

    RenderSystem::RenderSystem(
        Registry& registry,
        const RenderSystemDependencies& dependencies,
        RenderSystemConfiguration configuration
    )
        : impl_(std::make_unique<Impl>(registry, dependencies, std::move(configuration)))
    {
    }

    RenderSystem::~RenderSystem() noexcept = default;

    AttachmentId RenderSystem::attachment(SceneViewId id) const noexcept
    {
        const auto found = impl_->observations.find(id);
        return found == impl_->observations.end() ? AttachmentId{} : found->second.attachment;
    }

    RenderError RenderSystem::viewError(SceneViewId id) const noexcept
    {
        const auto found = impl_->observations.find(id);
        return found == impl_->observations.end() ? RenderError{} : found->second.error;
    }

    RenderError RenderSystem::assetError(Entity entity) const noexcept
    {
        const auto found = impl_->meshes.find(entityBits(entity));
        return found == impl_->meshes.end() ? RenderError{} : found->second.error;
    }

    bool RenderSystem::visualReady(Entity entity) const noexcept
    {
        const auto found = impl_->meshes.find(entityBits(entity));
        return found != impl_->meshes.end() && !found->second.instance.isNull() &&
               found->second.error.type == 0 && !found->second.replacement &&
               !impl_->structure.contains(entity) && !impl_->pending_instances.contains(entityBits(entity));
    }

    bool RenderSystem::viewRetired(SceneViewId id) const noexcept
    {
        return !impl_->view_inputs.findView(id) && !impl_->observations.contains(id) && !impl_->transaction_pending;
    }

    SceneStageResult RenderSystem::maintain(SceneStageContext& context) noexcept
    {
        auto& self = *impl_;
        if (context.stop.stop_requested())
        {
            if (self.transaction_pending)
            {
                return ESceneProgress::PENDING;
            }
            if (!self.closing)
            {
                self.closing = true;
                self.error = {}; // Structural failure does not discharge its created handles.
            }
            if (self.error.type != 0 && self.error.type != kRuntimeCapacity)
            {
                return failure(self.error);
            }
            const bool has_resources =
                !self.scene.isNull() || !self.observations.empty() || !self.meshes.empty() || !self.materials.empty();
            if (!has_resources)
            {
                return ESceneProgress::COMPLETE;
            }
            self.startTransaction(self.closeScene());
            return ESceneProgress::PENDING;
        }
        if (self.error.type != 0)
        {
            return failure(self.error);
        }
        const auto& inputs = self.view_inputs;
        const bool changed_inputs = self.camera_dirty || self.view_revision != inputs.revision() ||
                                    self.resynchronization != self.client.resynchronization();
        context.publication_needed |= !self.structure.empty() || !self.transformed.empty() ||
                                      !self.light_changes.empty() || changed_inputs || self.transaction_pending;
        if (self.transaction_pending || !context.allow_structure)
        {
            return ESceneProgress::COMPLETE;
        }
        const bool changed_views = self.view_revision != inputs.revision();
        self.rebuild_requested |=
            (changed_views && self.needsViewSynchronization()) || self.observed_topology != self.topology_revision;
        self.observed_topology = self.topology_revision;
        self.view_revision = inputs.revision();
        bool changed = !self.initialized || self.rebuild_requested;
        if (self.asset_revision != self.view_inputs.asset_revision)
        {
            self.asset_revision = self.view_inputs.asset_revision;
            for (const auto entity : self.registry.view<const Mesh3D>())
            {
                self.structureChanged(self.registry, entity);
            }
        }
        auto mesh_changes = self.collectMeshChanges();
        if (!mesh_changes)
        {
            return failure(mesh_changes.error());
        }
        changed |= *mesh_changes;
        std::vector<LightInput> light_inputs;
        auto light_changes = self.collectLightChanges(light_inputs);
        if (!light_changes)
        {
            return failure(light_changes.error());
        }
        changed |= *light_changes;
        changed |= !self.pending_instances.empty() && self.observed_assets != self.assets.revision();
        self.observed_assets = self.assets.revision();
        if (changed)
        {
            std::vector<InstanceInput> inputs;
            for (const auto bits : self.pending_instances)
            {
                const auto& entry = self.meshes.find(bits)->second;
                if (!entry.present || (!entry.instance.isNull() && !entry.replacement))
                {
                    continue;
                }
                const auto entity = static_cast<Entity>(bits);
                if (const auto* world = self.registry.try_get<const WorldTransform3D>(entity))
                {
                    InstanceInput value{bits};
                    const Eigen::Matrix4f matrix = world->value.matrix().cast<float>();
                    std::memcpy(value.description.transform.data(), matrix.data(), sizeof(TransformMatrix));
                    inputs.push_back(value);
                }
            }
            self.startTransaction(self.synchronizeStructure(std::move(inputs), std::move(light_inputs)));
        }
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult RenderSystem::prepareInputs(SceneStageContext&) noexcept
    {
        auto& self = *impl_;
        if (self.closing)
        {
            return ESceneProgress::COMPLETE;
        }
        self.transform_ids.clear();
        self.transform_entities.clear();
        for (auto bits : self.new_instances)
        {
            self.transformChanged(self.registry, static_cast<Entity>(bits));
        }
        self.new_instances.clear();
        if (self.resynchronization != self.client.resynchronization())
        {
            self.resynchronization = self.client.resynchronization();
            for (auto& [id, observation] : self.observations)
            {
                observation.parameters_changed = true;
            }
            for (const auto& [bits, entry] : self.meshes)
            {
                self.transformChanged(self.registry, static_cast<Entity>(bits));
            }
            for (const auto& [bits, entry] : self.lights)
            {
                self.lightChanged(self.registry, static_cast<Entity>(bits));
            }
        }
        for (const auto entity : self.transformed)
        {
            const auto bits = entityBits(entity);
            const auto entry = self.meshes.find(bits);
            if (entry == self.meshes.end() || !entry->second.present || entry->second.instance.isNull())
            {
                continue;
            }
            const auto* world = self.registry.try_get<const WorldTransform3D>(static_cast<Entity>(bits));
            if (!world)
            {
                continue;
            }
            self.transform_ids.push_back(entry->second.instance.local);
            self.transform_entities.push_back(entity);
        }
        self.transformed.erase(self.transformed.begin(), self.transformed.end());
        for (auto bits : self.new_lights)
        {
            self.lightChanged(self.registry, static_cast<Entity>(bits));
        }
        self.new_lights.clear();
        self.light_updates.clear();
        self.processed.clear();
        for (const auto entity : self.light_changes)
        {
            const auto found = self.lights.find(entityBits(entity));
            const auto* light = self.registry.valid(entity) ? self.registry.try_get<const Light3D>(entity) : nullptr;
            const auto* world =
                self.registry.valid(entity) ? self.registry.try_get<const WorldTransform3D>(entity) : nullptr;
            if (found == self.lights.end() || !light || !world)
            {
                if (found == self.lights.end())
                {
                    self.processed.push_back(entity);
                }
                continue;
            }
            if (!found->second.present || found->second.light.isNull())
            {
                continue;
            }
            auto value = lightValue(light->value, world->value);
            if (!value)
            {
                return failure(value.error());
            }
            self.light_updates.push_back({found->second.light, std::move(*value)});
            self.processed.push_back(entity);
        }
        for (auto entity : self.processed)
        {
            self.light_changes.erase(entity);
        }
        self.prepareViewParameters();
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult RenderSystem::publish(SceneStageContext&) noexcept
    {
        auto& self = *impl_;
        auto* frame = self.client.buildingFrame();
        if (!frame)
        {
            return ESceneProgress::PENDING;
        }
        if (self.closing)
        {
            return ESceneProgress::COMPLETE;
        }
        auto& client = self.client;
        if (!self.transform_ids.empty())
        {
            auto matrices = frame->allocate<TransformMatrix>(self.transform_entities.size());
            for (std::size_t i = 0; i < matrices.size(); ++i)
            {
                const auto& world = self.registry.get<const WorldTransform3D>(self.transform_entities[i]);
                Eigen::Map<Eigen::Matrix4f>{matrices[i].data()} = world.value.matrix().cast<float>();
            }
            const TransformBatch
                batch{self.instance_storage, ++self.sequence, frame->freeze(std::span(self.transform_ids)), matrices};
            if (auto appended = client.append(*frame, *self.transforms, batch); !appended)
            {
                return failure(appended.error());
            }
        }
        if (!self.light_updates.empty())
        {
            UpdateDirectLights::Request request{
                self.light_storage,
                self.sequence,
                frame->freeze(std::span(self.light_updates))
            };
            if (auto appended = client.append(*frame, *self.lighting, request); !appended)
            {
                return failure(appended.error());
            }
        }
        for (auto& [id, observation] : self.observations)
        {
            const bool can_publish = self.view_inputs.findView(id) && observation.error.type == 0 &&
                                     observation.parameters_changed && !observation.attachment.isNull();
            if (!can_publish)
            {
                continue;
            }
            AttachmentInputs inputs{
                observation.attachment,
                parameterBytes(observation.parameters),
                observation.adopted.viewport
            };
            if (auto appended = client.append(*frame, *self.attachment_inputs, std::move(inputs)); !appended)
            {
                return failure(appended.error());
            }
            observation.parameters_changed = false;
        }
        if (frame->group_ends.empty() || frame->group_ends.back() != frame->commands.size())
        {
            frame->group_ends.push_back(frame->commands.size());
        }
        return ESceneProgress::COMPLETE;
    }

    SceneSystemRegistration renderSystemRegistration() noexcept
    {
        static constexpr std::array requirements{SceneSystemRequirementSpec{
            "rendering",
            "lux.scene.rendering",
            cxx::typeToken<RenderSystemDependencies>(),
            false
        }};
        return {
            .type = system::systemTypeId(RenderSystem::Description.canonical_name),
            .cpp_type = cxx::typeToken<RenderSystem>(),
            .description = &RenderSystem::Description,
            .configuration = renderConfigurationCodec(),
            .requirements = requirements,
            .install = +[](SceneSystemInstaller& builder,
                           SceneSystemDescription input) noexcept -> cxx::expected<void, SceneSystemBuildFailure>
            {
                auto* dependencies = builder.require<RenderSystemDependencies>(input.instanceId(), "rendering");
                if (!dependencies)
                {
                    return cxx::unexpected(SceneSystemBuildFailure{ESceneSystemBuildError::MISSING_REQUIREMENT});
                }
                auto configuration = builder.decodeConfiguration<RenderSystemConfiguration>(input);
                if (!configuration || !validRenderConfiguration(*configuration))
                {
                    return cxx::unexpected(
                        SceneSystemBuildFailure{ESceneSystemBuildError::INVALID_DESCRIPTION, input.instanceId()}
                    );
                }
                auto instance = builder.emplaceSystem<RenderSystem>(
                    input.instanceId(),
                    builder.registry(),
                    *dependencies,
                    std::move(*configuration)
                );
                if (!instance)
                {
                    return cxx::unexpected(instance.error());
                }
                auto maintenance = builder.addMaintenanceTask<RenderSystem>(
                    input.instanceId(),
                    [](auto& system, auto& context) noexcept { return system.maintain(context); }
                );
                if (!maintenance)
                {
                    return maintenance;
                }
                auto prepared = builder.addStablePointTask<RenderSystem>(
                    input.instanceId(),
                    [](auto& system, auto& context) noexcept { return system.prepareInputs(context); }
                );
                if (!prepared)
                {
                    return prepared;
                }
                return builder.addPublicationTask<RenderSystem>(
                    input.instanceId(),
                    [](auto& system, auto& context) noexcept { return system.publish(context); }
                );
            }
        };
    }
} // namespace lux::scene
