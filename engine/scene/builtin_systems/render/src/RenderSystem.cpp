#include <lux/engine/scene/RenderSystem.hpp>
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
#include <entt/entity/sparse_set.hpp>
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
        };

        struct MeshEntry final
        {
            rdesc::MeshVisualDescription visual;
            std::uint64_t revision = 0;
            MeshInstanceId instance;
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
            CreateViewRequest view_request{scene};
            auto created_view = co_await request<CreateView>(client, view_request);
            if (created_view.error.type != 0)
            {
                co_return cxx::unexpected(created_view.error);
            }
            view = created_view.view;
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
            const bool missing_storage =
                instance_storage.isNull() || material_storage.isNull() || light_storage.isNull();
            const bool missing_feature = features.gbuffer.isNull() || features.lighting.isNull() ||
                                         features.tonemap.isNull() || features.visibility.isNull();
            if (missing_storage || missing_feature)
            {
                co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
            initialized = true;
            co_return RenderResult<void>{};
        }

        exec::task<RenderResult<void>> prepareMaterials() noexcept
        {
            for (auto pending = pending_materials.begin(); pending != pending_materials.end();)
            {
                const auto key = *pending++;
                auto& entry = materials.find(key)->second;
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
                        co_return cxx::unexpected(error);
                    }
                    continue;
                }
                // Incremental admission can stop at the actual Asset limit without reacquiring earlier slots.
                for (std::size_t i = entry.textures.size(); i < cooked->texture_slot_ids.size(); ++i)
                {
                    const RenderAssetVersion texture{cooked->texture_slot_ids[i], key.revision};
                    if (!texture.asset.isNull())
                    {
                        auto acquired = assets.acquireTexture(texture, source);
                        if (!acquired && acquired.error().type == kRuntimeCapacity)
                        {
                            break;
                        }
                        if (!acquired)
                        {
                            co_return cxx::unexpected(acquired.error());
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
                            co_return cxx::unexpected(error);
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
                    co_return cxx::unexpected(created.error);
                }
                entry.material = created.material;
                entry.program_hash = programHash(cooked->programs);
                pending_materials.erase(key);
            }
            co_return RenderResult<void>{};
        }

        exec::task<RenderResult<void>> rebuildEffect() noexcept
        {
            GBufferFeatureConfig surface{description};
            surface.visible_capacity =
                std::bit_ceil(std::max<std::uint32_t>(128, static_cast<std::uint32_t>(meshes.size())));
            std::unordered_multimap<std::uint64_t, std::size_t> group_indices;
            std::vector<const rdesc::MaterialProgramSet*> group_programs;
            for (const auto& [entity, entry] : meshes)
            {
                if (!entry.present || entry.instance.isNull())
                {
                    continue;
                }
                const auto& mesh = *assets.mesh({entry.visual.mesh, entry.revision});
                const auto& material = materials.find({entry.visual.material, entry.revision})->second;
                const auto& program = assets.material({entry.visual.material, entry.revision})->programs;
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
                // Engine Camera uses a right-handed, image-down projection; compensate its Y flip once.
                group.front_counter_clockwise = false;
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
            std::vector<MeshInstanceDrawGroup> selection;
            selection.reserve(surface.groups.size());
            for (const auto& group : surface.groups)
            {
                selection.push_back(group.group);
            }
            const bool unchanged_recipe = !rebuild_requested && !attachment.isNull() &&
                                          surface.visible_capacity <= visible_capacity && selection == published_groups;
            if (unchanged_recipe)
            {
                co_return RenderResult<void>{};
            }
            const auto capacity = std::max(surface.visible_capacity, visible_capacity);
            auto recipe = detail::makeSceneEffect(description, std::move(surface.groups), capacity, features);
            if (!recipe)
            {
                co_return cxx::unexpected(recipe.error());
            }
            auto input = std::move(*recipe);
            auto compiled = co_await request<CreateEffect>(client, input);
            if (compiled.error.type != 0)
            {
                co_return cxx::unexpected(compiled.error);
            }
            effect_to_release = compiled.effect;
            AttachmentRequest
                attach{scene, view, target, compiled.effect, parameterBytes(parameters), viewport, {}, attachment};
            const auto adopted = co_await request<Attach>(client, attach);
            if (adopted.error.type != 0)
            {
                EffectRequest discard{effect_to_release};
                const auto removed = co_await request<RemoveEffect>(client, discard);
                if (removed.error.type != 0)
                {
                    reportRenderFailure(removed.error, "Discard candidate Effect");
                }
                else
                {
                    effect_to_release = {};
                }
                co_return cxx::unexpected(adopted.error);
            }
            std::swap(effect, effect_to_release);
            attachment = adopted.attachment;
            published_groups = std::move(selection);
            visible_capacity = capacity;
            if (!effect_to_release.isNull())
            {
                EffectRequest remove{effect_to_release};
                auto removed = co_await request<RemoveEffect>(client, remove);
                if (removed.error.type != 0)
                {
                    co_return cxx::unexpected(removed.error);
                }
                effect_to_release = {};
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
            bool changed = attachment.isNull() || rebuild_requested;
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
            }
            if (!light_inputs.empty())
            {
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
            auto prepared = co_await prepareMaterials();
            if (!prepared)
            {
                co_return prepared;
            }
            std::vector<MeshInstanceDescription> descriptions;
            std::vector<std::uint64_t> entities;
            for (auto& input : inputs)
            {
                auto& entry = meshes.find(input.entity)->second;
                const auto* mesh = assets.mesh({entry.visual.mesh, entry.revision});
                if (!mesh)
                {
                    const auto error = assets.failure({entry.visual.mesh, entry.revision});
                    if (error.type != 0)
                    {
                        co_return cxx::unexpected(error);
                    }
                }
                const auto material = materials.find({entry.visual.material, entry.revision});
                if (!entry.present || !entry.instance.isNull() || !mesh || material->second.material.isNull())
                {
                    continue;
                }
                input.description.mesh = mesh->mesh;
                input.description.material = material->second.material;
                descriptions.push_back(input.description);
                entities.push_back(input.entity);
            }
            if (!descriptions.empty())
            {
                MeshInstancesCreate create{instance_storage, descriptions};
                auto created = co_await request<CreateMeshInstances>(client, create);
                if (created.error.type != 0)
                {
                    co_return cxx::unexpected(created.error);
                }
                for (std::size_t i = 0; i < entities.size(); ++i)
                {
                    meshes.find(entities[i])->second.instance = created.instances[i];
                    new_instances.push_back(entities[i]);
                    pending_instances.erase(entities[i]);
                }
                changed = true;
            }
            if (changed && !closing)
            {
                auto rebuilt = co_await rebuildEffect();
                if (!rebuilt)
                {
                    co_return rebuilt;
                }
                rebuild_requested = false;
            }
            for (auto entry = meshes.begin(); entry != meshes.end();)
            {
                if (entry->second.present)
                {
                    ++entry;
                    continue;
                }
                assets.releaseMesh({entry->second.visual.mesh, entry->second.revision});
                --materials.find({entry->second.visual.material, entry->second.revision})->second.references;
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
                view = {};
                attachment = {};
            }
            for (auto* handle : {&effect_to_release, &effect})
            {
                if (handle->isNull())
                {
                    continue;
                }
                EffectRequest remove{*handle};
                auto result = co_await request<RemoveEffect>(client, remove);
                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
                *handle = {};
            }
            while (!meshes.empty())
            {
                const auto entry = meshes.begin();
                assets.releaseMesh({entry->second.visual.mesh, entry->second.revision});
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
                if (current != meshes.end())
                {
                    if (visible && current->second.present && current->second.visual == mesh->value &&
                        current->second.revision == asset_revision)
                    {
                        processed.push_back(entity);
                        continue;
                    }
                    current->second.present = false;
                    changed = true;
                    if (visible)
                    {
                        continue;
                    } // Re-admit the new binding after the old one is retired.
                }
                else if (visible)
                {
                    const RenderAssetVersion mesh_key{mesh->value.mesh, asset_revision},
                        material_key{mesh->value.material, asset_revision};
                    auto mesh_acquired = assets.acquireMesh(mesh_key, source);
                    if (!mesh_acquired && mesh_acquired.error().type == kRuntimeCapacity)
                    {
                        continue;
                    }
                    if (!mesh_acquired)
                    {
                        return cxx::unexpected(mesh_acquired.error());
                    }
                    auto material = materials.find(material_key);
                    if (material == materials.end())
                    {
                        auto acquired = assets.acquireMaterial(material_key, source);
                        if (!acquired)
                        {
                            assets.releaseMesh(mesh_key);
                            if (acquired.error().type == kRuntimeCapacity)
                            {
                                continue;
                            }
                            return cxx::unexpected(acquired.error());
                        }
                        material = materials.emplace(material_key, MaterialEntry{}).first;
                        pending_materials.insert(material_key);
                    }
                    ++material->second.references;
                    meshes.emplace(bits, MeshEntry{mesh->value, asset_revision});
                    pending_instances.insert(bits);
                    changed = true;
                }
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

        SceneStageResult prepareViewParameters() noexcept
        {
            const auto& inputs = view_inputs;
            if (!inputs.description.width || !inputs.description.height)
            {
                return ESceneProgress::COMPLETE;
            }
            if (!camera_dirty && view_revision == inputs.revision)
            {
                return ESceneProgress::COMPLETE;
            }
            Entity primary = NullEntity;
            for (const auto entity : registry.view<const Camera, const WorldTransform3D>())
            {
                if (!registry.get<const Camera>(entity).primary)
                {
                    continue;
                }
                if (primary != NullEntity)
                {
                    return failure(RenderError{
                        kCommandContract,
                        {static_cast<std::uint64_t>(ECameraError::MULTIPLE_PRIMARY_CAMERAS)}
                    });
                }
                primary = entity;
            }
            if (primary == NullEntity)
            {
                return failure(
                    RenderError{kCommandContract, {static_cast<std::uint64_t>(ECameraError::NO_PRIMARY_CAMERA)}}
                );
            }
            {
                const auto entity = primary;
                const auto& camera = registry.get<const Camera>(entity);
                const auto& world = registry.get<const WorldTransform3D>(entity).value;
                auto projection =
                    cameraProjection(camera, double(inputs.description.width) / inputs.description.height);
                auto view = cameraView(world, Eigen::Vector3d::Zero());
                if (!projection || !view)
                {
                    return failure(RenderError{kCommandContract});
                }
                SceneParameters parameters;
                const Eigen::Matrix4f v = view->cast<float>(), p = projection->cast<float>();
                const Eigen::Matrix4f inverse = (p * v).inverse();
                std::memcpy(parameters.mesh.view.data(), v.data(), sizeof(v));
                std::memcpy(parameters.mesh.projection.data(), p.data(), sizeof(p));
                std::memcpy(parameters.lighting.inverse_view_projection.data(), inverse.data(), sizeof(inverse));
                const auto position = world.translation();
                parameters.lighting
                    .camera_position = {float(position.x()), float(position.y()), float(position.z()), 1};
                parameters.lighting.background = inputs.background;
                parameters.lighting
                    .viewport = {inputs.viewport.x, inputs.viewport.y, inputs.viewport.width, inputs.viewport.height};
                parameters.tone.exposure = inputs.exposure;
                parameters_changed |= std::memcmp(&parameters, &parameters, sizeof(parameters)) != 0;
                parameters = parameters;
                camera_dirty = false;
                view_revision = inputs.revision;
            }
            return ESceneProgress::COMPLETE;
        }

        Impl(Registry& registry, const RenderSystemDependencies& dependencies)
            : registry(registry), view_inputs(dependencies.inputs), client(dependencies.client),
              execution(dependencies.execution), assets(dependencies.assets), source(dependencies.source),
              mesh_added(registry.on_construct<Mesh3D>().connect<&Impl::structureChanged>(*this)),
              mesh_updated(registry.on_update<Mesh3D>().connect<&Impl::structureChanged>(*this)),
              mesh_removed(registry.on_destroy<Mesh3D>().connect<&Impl::structureChanged>(*this)),
              pose_added(registry.on_construct<WorldTransform3D>().connect<&Impl::poseAdded>(*this)),
              pose_updated(registry.on_update<WorldTransform3D>().connect<&Impl::transformChanged>(*this)),
              pose_removed(registry.on_destroy<WorldTransform3D>().connect<&Impl::poseRemoved>(*this)),
              camera_added(registry.on_construct<Camera>().connect<&Impl::cameraChanged>(*this)),
              camera_updated(registry.on_update<Camera>().connect<&Impl::cameraChanged>(*this)),
              camera_removed(registry.on_destroy<Camera>().connect<&Impl::cameraChanged>(*this)),
              light_added(registry.on_construct<Light3D>().connect<&Impl::lightChanged>(*this)),
              light_updated(registry.on_update<Light3D>().connect<&Impl::lightChanged>(*this)),
              light_removed(registry.on_destroy<Light3D>().connect<&Impl::lightChanged>(*this))
        {
            for (const auto entity : registry.view<const Mesh3D>())
            {
                structure.push(entity);
            }
            for (const auto entity : registry.view<const Light3D>())
            {
                light_changes.push(entity);
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
            if (transaction_pending || !scene.isNull() || !effect.isNull() || !effect_to_release.isNull() ||
                !meshes.empty() || !materials.empty())
            {
                std::terminate(); // Retirement maintenance finishes before the System is destroyed.
            }
        }

        void structureChanged(Registry&, Entity entity) noexcept
        {
            if (!structure.contains(entity))
            {
                structure.push(entity);
            }
        }

        void poseAdded(Registry& registry, Entity entity) noexcept
        {
            structureChanged(registry, entity);
            transformChanged(registry, entity);
        }

        void cameraChanged(Registry&, Entity) noexcept
        {
            camera_dirty = true;
        }

        void poseRemoved(Registry& registry, Entity entity) noexcept
        {
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
                light_changes.push(entity);
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
                transformed.push(entity);
            }
        }

        Registry& registry;
        SceneRenderInputs& view_inputs;
        RenderRequester& client;
        execution::ExecutionRuntime& execution;
        RenderAssets& assets;
        process::asset_loading::AssetReadPort source;
        std::unordered_map<std::uint64_t, MeshEntry> meshes;
        std::vector<std::uint64_t> new_instances, new_lights;
        std::unordered_set<std::uint64_t> pending_instances;
        std::unordered_map<std::uint64_t, LightEntry> lights;
        std::map<RenderAssetVersion, MaterialEntry> materials;
        std::set<RenderAssetVersion> pending_materials;
        SceneId scene;
        ViewId view;
        EffectId effect;
        // Known candidate/replaced Effect remains here until release is acknowledged, including failed Attach.
        EffectId effect_to_release;
        AttachmentId attachment;
        StorageId instance_storage, material_storage, light_storage;
        detail::SceneEffectFeatures features;
        std::vector<MeshInstanceDrawGroup> published_groups;
        std::uint32_t visible_capacity = 0;
        TargetId target;
        TargetDesc description;
        RasterViewport viewport;
        SceneParameters parameters;
        RenderError error;
        bool initialized = false, transaction_pending = false, closing = false;
        bool rebuild_requested = false;

        entt::basic_sparse_set<Entity> structure, transformed, light_changes;
        std::vector<Entity> processed;
        std::vector<cxx::SlotKey<MeshInstanceTag>> transform_ids;
        std::vector<Entity> transform_entities;
        std::vector<DirectLightUpdate> light_updates;
        std::optional<BoundCommand<UpdateTransforms>> transforms;
        std::optional<BoundCommand<UpdateDirectLights>> lighting;
        std::optional<BoundCommand<UpdateAttachmentInputs>> attachment_inputs;
        std::uint64_t sequence = 0, observed_assets = 0, resynchronization = 0;
        bool parameters_changed = true, camera_dirty = true;
        std::uint64_t view_revision = UINT64_MAX, asset_revision = 0;
        entt::scoped_connection mesh_added, mesh_updated, mesh_removed, pose_added, pose_updated, pose_removed;
        entt::scoped_connection camera_added, camera_updated, camera_removed;
        entt::scoped_connection light_added, light_updated, light_removed;
    };

    RenderSystem::RenderSystem(Registry& registry, const RenderSystemDependencies& dependencies)
        : impl_(std::make_unique<Impl>(registry, dependencies))
    {
    }

    RenderSystem::~RenderSystem() noexcept = default;

    AttachmentId RenderSystem::attachment() const noexcept
    {
        return impl_->attachment;
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
            const bool has_resources = !self.scene.isNull() || !self.effect.isNull() ||
                                       !self.effect_to_release.isNull() || !self.meshes.empty() ||
                                       !self.materials.empty();
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
        const bool changed_inputs = self.camera_dirty || self.view_revision != inputs.revision ||
                                    self.resynchronization != self.client.resynchronization();
        context.publication_needed |= !self.structure.empty() || !self.transformed.empty() ||
                                      !self.light_changes.empty() || changed_inputs || self.transaction_pending;
        if (self.transaction_pending || !context.allow_structure || self.view_inputs.target.isNull())
        {
            return ESceneProgress::COMPLETE;
        }
        self.rebuild_requested |= self.initialized && (self.target != self.view_inputs.target ||
                                                       self.description != self.view_inputs.description);
        self.target = self.view_inputs.target;
        self.description = self.view_inputs.description;
        self.viewport = self.view_inputs.viewport;
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
                if (!entry.present || !entry.instance.isNull())
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
            self.parameters_changed = true;
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
        self.transformed.clear();
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
        return self.prepareViewParameters();
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
        if (self.parameters_changed && !self.attachment.isNull())
        {
            AttachmentInputs inputs{self.attachment, parameterBytes(self.parameters), self.view_inputs.viewport};
            if (auto appended = client.append(*frame, *self.attachment_inputs, std::move(inputs)); !appended)
            {
                return failure(appended.error());
            }
            self.parameters_changed = false;
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
            .requirements = requirements,
            .install = +[](SceneSystemInstaller& builder,
                           SceneSystemDescription input) noexcept -> cxx::expected<void, SceneSystemBuildFailure>
            {
                auto* dependencies = builder.require<RenderSystemDependencies>(input.instanceId(), "rendering");
                if (!dependencies)
                {
                    return cxx::unexpected(SceneSystemBuildFailure{ESceneSystemBuildError::MISSING_REQUIREMENT});
                }
                auto instance =
                    builder.emplaceSystem<RenderSystem>(input.instanceId(), builder.registry(), *dependencies);
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
