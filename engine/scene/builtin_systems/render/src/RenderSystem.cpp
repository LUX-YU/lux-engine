#include <lux/engine/scene/RenderSystem.hpp>

#include <exec/task.hpp>
#include <exec/variant_sender.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/render/features/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/FeatureGraphOps.hpp>
#include <lux/engine/render/features/GBufferFeature.hpp>
#include <lux/engine/render/features/MeshVisibilityFeature.hpp>
#include <lux/engine/render/features/TonemapFeature.hpp>
#include <lux/engine/render/storage/DirectLighting.hpp>
#include <lux/engine/rendering/RenderAssets.hpp>
#include <lux/engine/rendering/RenderDiagnostics.hpp>
#include <lux/engine/rendering/RenderExecution.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

#include <bit>
#include <cstring>
#include <entt/entity/sparse_set.hpp>
#include <map>
#include <unordered_map>

namespace lux::scene
{
    using namespace render;
    using namespace rendering;
    using namespace simulation::ecs;

    namespace
    {
        struct SceneParameters final
        {
            MeshViewParameters mesh;
            DeferredLightingParameters lighting;
            TonemapParameters tone;
            std::array<float, 3> padding{};
        };

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

        struct MaterialEntry final
        {
            MaterialId material;
            std::vector<RenderAssetVersion> textures;
            std::size_t references = 0;
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

        // This state may outlive the ECS system only while a finite protocol transaction settles.
        // It contains no Registry/SceneSystem pointer, CPU transform mirror, thread or operation owner.
        struct SceneRendering final
        {
            SceneRendering(RenderExecution& rendering, RenderAssets& assets, execution::TaskScope& tasks) noexcept
                : rendering(rendering), assets(assets), tasks(tasks)
            {
            }

            exec::task<RenderResult<void>> initialize() noexcept
            {
                auto& client = rendering.mainClient();
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
                const auto features = co_await request<ListFeatures>(client, empty);
                if (features.error.type != 0)
                {
                    co_return cxx::unexpected(features.error);
                }
                for (const auto& feature : features.features)
                {
                    if (feature.name == GBufferFeatureDescriptor::name.name())
                    {
                        gbuffer = feature.id;
                    }
                    if (feature.name == DeferredLightingFeatureDescriptor::name.name())
                    {
                        lighting = feature.id;
                    }
                    if (feature.name == TonemapFeatureDescriptor::name.name())
                    {
                        tonemap = feature.id;
                    }
                    if (feature.name == MeshVisibilityFeatureDescriptor::name.name())
                    {
                        visibility = feature.id;
                    }
                }
                const bool missing_storage =
                    instance_storage.isNull() || material_storage.isNull() || light_storage.isNull();
                const bool missing_feature =
                    gbuffer.isNull() || lighting.isNull() || tonemap.isNull() || visibility.isNull();
                if (missing_storage || missing_feature)
                {
                    co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
                }
                initialized = true;
                co_return RenderResult<void>{};
            }

            exec::task<RenderResult<void>> prepareMaterials() noexcept
            {
                auto& client = rendering.mainClient();
                for (auto& [key, entry] : materials)
                {
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
                            auto acquired = assets.acquireTexture(texture);
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
                }
                co_return RenderResult<void>{};
            }

            exec::task<RenderResult<void>> rebuildEffect() noexcept
            {
                const auto layout = sceneParameterLayout();
                GBufferFeatureConfig surface{description};
                surface.input_layout = layout;
                surface.visible_capacity =
                    std::bit_ceil(std::max<std::uint32_t>(128, static_cast<std::uint32_t>(meshes.size())));
                for (const auto& [entity, entry] : meshes)
                {
                    if (!entry.present || entry.instance.isNull())
                    {
                        continue;
                    }
                    const auto& mesh = *assets.mesh({entry.visual.mesh, entry.revision});
                    const auto& material = materials.find({entry.visual.material, entry.revision})->second;
                    const auto& program = assets.material({entry.visual.material, entry.revision})->programs;
                    const bool already_grouped = std::ranges::any_of(
                        surface.groups,
                        [&](const RasterMeshGroup& group)
                        { return group.group.mesh == mesh.mesh && group.program == program; }
                    );
                    if (already_grouped)
                    {
                        continue;
                    }
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
                auto surface_config = ConfigValue::make(surface);
                auto light_config = ConfigValue::make(light);
                auto tone_config = ConfigValue::make(tone);
                auto visibility_config = ConfigValue::make(selection);
                if (!surface_config || !light_config || !tone_config || !visibility_config)
                {
                    co_return cxx::unexpected(RenderError{kFeatureContract});
                }
                input.definition.features.push_back({gbuffer, "surfaces", std::move(*surface_config)});
                input.definition.features.push_back({lighting, "lighting", std::move(*light_config)});
                input.definition.features.push_back({tonemap, "tonemap", std::move(*tone_config)});
                for (auto name : {"albedo_metallic", "normal_roughness", "emissive_ao", "depth"})
                {
                    input.definition.connections.push_back({{0, name}, {1, name}});
                }
                input.definition.connections.push_back({{1, "hdr_color"}, {2, "hdr_color"}});
                input.definition.outputs.push_back({{2, "color"}, GraphOutputRequirements{description.format, 1}});
                if (!selection.groups.empty())
                {
                    input.definition.features.push_back({visibility, "visibility", std::move(*visibility_config)});
                    for (std::size_t i = 0; i < selection.groups.size(); ++i)
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
                auto& client = rendering.mainClient();
                auto compiled = co_await request<CreateEffect>(client, input);
                if (compiled.error.type != 0)
                {
                    co_return cxx::unexpected(compiled.error);
                }
                AttachmentRequest
                    attach{scene, view, target, compiled.effect, parameterBytes(parameters), viewport, {}, attachment};
                const auto adopted = co_await request<Attach>(client, attach);
                if (adopted.error.type != 0)
                {
                    EffectRequest discard{compiled.effect};
                    const auto removed = co_await request<RemoveEffect>(client, discard);
                    if (removed.error.type != 0)
                    {
                        reportRenderFailure(removed.error, "Discard candidate Effect");
                    }
                    co_return cxx::unexpected(adopted.error);
                }
                const auto previous = effect;
                effect = compiled.effect;
                attachment = adopted.attachment;
                if (!previous.isNull())
                {
                    EffectRequest remove{previous};
                    auto removed = co_await request<RemoveEffect>(client, remove);
                    if (removed.error.type != 0)
                    {
                        co_return cxx::unexpected(removed.error);
                    }
                }
                co_return RenderResult<void>{};
            }

            exec::task<RenderResult<void>> update(
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
                auto& client = rendering.mainClient();
                bool changed = attachment.isNull();
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
                    entry = materials.erase(entry);
                }
                co_return RenderResult<void>{};
            }

            exec::task<RenderResult<void>> close() noexcept
            {
                auto& client = rendering.mainClient();
                if (!scene.isNull())
                {
                    SceneRequest remove{scene};
                    auto result = co_await request<RemoveScene>(client, remove);
                    if (result.error.type != 0)
                    {
                        co_return cxx::unexpected(result.error);
                    }
                    scene = {};
                    attachment = {};
                }
                if (!effect.isNull())
                {
                    EffectRequest remove{effect};
                    auto result = co_await request<RemoveEffect>(client, remove);
                    if (result.error.type != 0)
                    {
                        co_return cxx::unexpected(result.error);
                    }
                    effect = {};
                }
                for (const auto& [entity, entry] : meshes)
                {
                    assets.releaseMesh({entry.visual.mesh, entry.revision});
                }
                for (const auto& [key, entry] : materials)
                {
                    if (!entry.material.isNull())
                    {
                        auto material = entry.material;
                        auto removed = co_await request<ReleaseMaterial>(client, material);
                        if (removed.error.type != 0)
                        {
                            co_return cxx::unexpected(removed.error);
                        }
                    }
                    for (const auto texture : entry.textures)
                    {
                        if (!texture.asset.isNull())
                        {
                            assets.releaseTexture(texture);
                        }
                    }
                    assets.releaseMaterial(key);
                }
                meshes.clear();
                materials.clear();
                lights.clear();
                co_return RenderResult<void>{};
            }

            RenderExecution& rendering;
            RenderAssets& assets;
            execution::TaskScope& tasks;
            std::unordered_map<std::uint64_t, MeshEntry> meshes;
            std::vector<std::uint64_t> new_instances, new_lights;
            std::unordered_map<std::uint64_t, LightEntry> lights;
            std::map<RenderAssetVersion, MaterialEntry> materials;
            SceneId scene;
            ViewId view;
            EffectId effect;
            AttachmentId attachment;
            StorageId instance_storage, material_storage, light_storage;
            FeatureId gbuffer, lighting, tonemap, visibility;
            TargetId target;
            TargetDesc description;
            RasterViewport viewport;
            SceneParameters parameters;
            RenderError error;
            bool initialized = false, busy = false, closing = false;
        };

        exec::task<RenderResult<void>> updateScene(
            std::shared_ptr<SceneRendering> state,
            std::vector<InstanceInput> inputs,
            std::vector<LightInput> lights
        ) noexcept
        {
            RenderResult<void> result;
            if (!state->closing)
            {
                result = co_await state->update(std::move(inputs), std::move(lights));
            }
            if (state->closing)
            {
                auto closed = co_await state->close();
                if (!closed)
                {
                    result = closed;
                }
            }
            co_return result;
        }

        void startUpdate(
            std::shared_ptr<SceneRendering> state,
            std::vector<InstanceInput> inputs,
            std::vector<LightInput> lights = {}
        ) noexcept
        {
            state->busy = true;
            auto task = state->tasks.submit(
                {.name = "Synchronize Render Scene", .category = "render.scene"},
                [state, inputs = std::move(inputs), lights = std::move(lights)](execution::TaskReporter
                ) mutable noexcept
                {
                    auto operation = stdexec::starts_on(
                        state->rendering.execution().mainScheduler(),
                        updateScene(state, std::move(inputs), std::move(lights))
                    );
                    // Scene destruction cancels adoption, not the real created handles. This finite
                    // protocol transaction must reach its cleanup path while Main frames continue.
                    auto settled = execution::taskResult(stdexec::write_env(
                        std::move(operation),
                        stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                    ));
                    return stdexec::then(
                        std::move(settled),
                        [state](auto result) noexcept
                        {
                            state->busy = false;
                            if (!result)
                            {
                                state->error = result.error().domainFailure() ? *result.error().domainFailure()
                                                                              : RenderError{kRuntimeState};
                                reportRenderFailure(state->error, "Synchronize Render Scene");
                            }
                            return execution::taskState(result);
                        }
                    );
                }
            );
            if (!task)
            {
                state->busy = false;
                state->error = RenderError{kRuntimeCapacity};
                reportRenderFailure(state->error, "Admit Render Scene transaction");
            }
        }

        SceneStageResult failure(RenderError error) noexcept
        {
            return cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, error});
        }
    } // namespace

    struct RenderSystem::Impl final
    {
        Impl(Registry& registry, SceneRenderServices& services)
            : registry(registry), services(services),
              state(std::make_shared<SceneRendering>(services.rendering, services.assets, services.tasks)),
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
            auto transforms_command = services.rendering.mainClient().bind<UpdateTransforms>();
            auto lighting_command = services.rendering.mainClient().bind<UpdateDirectLights>();
            auto inputs_command = services.rendering.mainClient().bind<UpdateAttachmentInputs>();
            if (!transforms_command || !lighting_command || !inputs_command)
            {
                state->error = RenderError{kRuntimeUnsupported};
                return;
            }
            transforms = *transforms_command;
            lighting = *lighting_command;
            attachment_inputs = *inputs_command;
        }

        ~Impl() noexcept
        {
            state->closing = true;
            if (!state->busy)
            {
                startUpdate(state, {});
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
        SceneRenderServices& services;
        std::shared_ptr<SceneRendering> state;
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

    RenderSystem::RenderSystem(Registry& registry, SceneRenderServices& services)
        : impl_(std::make_unique<Impl>(registry, services))
    {
    }

    RenderSystem::~RenderSystem() noexcept = default;

    AttachmentId RenderSystem::attachment() const noexcept
    {
        return impl_->state->attachment;
    }

    SceneStageResult RenderSystem::maintain(SceneStageContext& context) noexcept
    {
        auto& self = *impl_;
        auto& state = *self.state;
        if (state.error.type != 0)
        {
            return failure(state.error);
        }
        const auto& inputs = self.services.inputs;
        const bool changed_inputs = self.camera_dirty || self.view_revision != inputs.revision ||
                                    self.resynchronization != inputs.resynchronization;
        context.publication_needed |= !self.structure.empty() || !self.transformed.empty() ||
                                      !self.light_changes.empty() || changed_inputs || state.busy;
        if (state.busy || self.services.inputs.target.isNull())
        {
            return ESceneProgress::COMPLETE;
        }
        state.target = self.services.inputs.target;
        state.description = self.services.inputs.description;
        state.viewport = self.services.inputs.viewport;
        bool changed = !state.initialized;
        if (self.asset_revision != self.services.inputs.asset_revision)
        {
            self.asset_revision = self.services.inputs.asset_revision;
            for (const auto entity : self.registry.view<const Mesh3D>())
            {
                self.structureChanged(self.registry, entity);
            }
        }
        self.processed.clear();
        for (const auto entity : self.structure)
        {
            const auto bits = entityBits(entity);
            const auto* mesh = self.registry.valid(entity) ? self.registry.try_get<const Mesh3D>(entity) : nullptr;
            const bool visible = mesh && mesh->value.visible && self.registry.all_of<WorldTransform3D>(entity);
            auto current = state.meshes.find(bits);
            if (current != state.meshes.end())
            {
                if (visible && current->second.present && current->second.visual == mesh->value &&
                    current->second.revision == self.asset_revision)
                {
                    self.processed.push_back(entity);
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
                const RenderAssetVersion mesh_key{mesh->value.mesh, self.asset_revision},
                    material_key{mesh->value.material, self.asset_revision};
                auto mesh_acquired = state.assets.acquireMesh(mesh_key);
                if (!mesh_acquired && mesh_acquired.error().type == kRuntimeCapacity)
                {
                    continue;
                }
                if (!mesh_acquired)
                {
                    return failure(mesh_acquired.error());
                }
                auto material = state.materials.find(material_key);
                if (material == state.materials.end())
                {
                    auto acquired = state.assets.acquireMaterial(material_key);
                    if (!acquired)
                    {
                        state.assets.releaseMesh(mesh_key);
                        if (acquired.error().type == kRuntimeCapacity)
                        {
                            continue;
                        }
                        return failure(acquired.error());
                    }
                    material = state.materials.emplace(material_key, MaterialEntry{}).first;
                }
                ++material->second.references;
                state.meshes.emplace(bits, MeshEntry{mesh->value, self.asset_revision});
                changed = true;
            }
            self.processed.push_back(entity);
        }
        for (auto entity : self.processed)
        {
            self.structure.erase(entity);
        }
        std::vector<LightInput> light_inputs;
        for (const auto entity : self.light_changes)
        {
            const auto bits = entityBits(entity);
            const auto* light = self.registry.valid(entity) ? self.registry.try_get<const Light3D>(entity) : nullptr;
            const auto* pose =
                self.registry.valid(entity) ? self.registry.try_get<const WorldTransform3D>(entity) : nullptr;
            auto found = state.lights.find(bits);
            if (!light || !pose)
            {
                if (found != state.lights.end())
                {
                    found->second.present = false;
                    changed = true;
                }
                continue;
            }
            if (found == state.lights.end())
            {
                found = state.lights.emplace(bits, LightEntry{{}, light->value.type}).first;
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
                    return failure(value.error());
                }
                light_inputs.push_back({bits, std::move(*value)});
                changed = true;
            }
        }
        changed |= self.observed_assets != state.assets.revision();
        self.observed_assets = state.assets.revision();
        if (changed)
        {
            std::vector<InstanceInput> inputs;
            for (const auto& [bits, entry] : state.meshes)
            {
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
            startUpdate(self.state, std::move(inputs), std::move(light_inputs));
        }
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult RenderSystem::prepareInputs(SceneStageContext&) noexcept
    {
        auto& self = *impl_;
        if (self.state->busy)
        {
            return ESceneProgress::COMPLETE;
        }
        self.transform_ids.clear();
        self.transform_entities.clear();
        for (auto bits : self.state->new_instances)
        {
            self.transformChanged(self.registry, static_cast<Entity>(bits));
        }
        self.state->new_instances.clear();
        if (self.resynchronization != self.services.inputs.resynchronization)
        {
            self.resynchronization = self.services.inputs.resynchronization;
            self.parameters_changed = true;
            for (const auto& [bits, entry] : self.state->meshes)
            {
                self.transformChanged(self.registry, static_cast<Entity>(bits));
            }
            for (const auto& [bits, entry] : self.state->lights)
            {
                self.lightChanged(self.registry, static_cast<Entity>(bits));
            }
        }
        for (const auto entity : self.transformed)
        {
            const auto bits = entityBits(entity);
            const auto entry = self.state->meshes.find(bits);
            if (entry == self.state->meshes.end() || !entry->second.present || entry->second.instance.isNull())
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
        for (auto bits : self.state->new_lights)
        {
            self.lightChanged(self.registry, static_cast<Entity>(bits));
        }
        self.state->new_lights.clear();
        self.light_updates.clear();
        self.processed.clear();
        for (const auto entity : self.light_changes)
        {
            const auto found = self.state->lights.find(entityBits(entity));
            const auto* light = self.registry.valid(entity) ? self.registry.try_get<const Light3D>(entity) : nullptr;
            const auto* world =
                self.registry.valid(entity) ? self.registry.try_get<const WorldTransform3D>(entity) : nullptr;
            if (found == self.state->lights.end() || !light || !world)
            {
                if (found == self.state->lights.end())
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
        const auto& inputs = self.services.inputs;
        if (!inputs.description.width || !inputs.description.height)
        {
            return ESceneProgress::COMPLETE;
        }
        if (!self.camera_dirty && self.view_revision == inputs.revision)
        {
            return ESceneProgress::COMPLETE;
        }
        Entity primary = NullEntity;
        for (const auto entity : self.registry.view<const Camera, const WorldTransform3D>())
        {
            if (!self.registry.get<const Camera>(entity).primary)
            {
                continue;
            }
            if (primary != NullEntity)
            {
                return failure(
                    RenderError{kCommandContract, {static_cast<std::uint64_t>(ECameraError::MULTIPLE_PRIMARY_CAMERAS)}}
                );
            }
            primary = entity;
        }
        if (primary == NullEntity)
        {
            return failure(RenderError{kCommandContract, {static_cast<std::uint64_t>(ECameraError::NO_PRIMARY_CAMERA)}}
            );
        }
        {
            const auto entity = primary;
            const auto& camera = self.registry.get<const Camera>(entity);
            const auto& world = self.registry.get<const WorldTransform3D>(entity).value;
            auto projection = cameraProjection(camera, double(inputs.description.width) / inputs.description.height);
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
            parameters.lighting.camera_position = {float(position.x()), float(position.y()), float(position.z()), 1};
            parameters.lighting.background = inputs.background;
            parameters.lighting
                .viewport = {inputs.viewport.x, inputs.viewport.y, inputs.viewport.width, inputs.viewport.height};
            parameters.tone.exposure = inputs.exposure;
            self.parameters_changed |= std::memcmp(&parameters, &self.state->parameters, sizeof(parameters)) != 0;
            self.state->parameters = parameters;
            self.camera_dirty = false;
            self.view_revision = inputs.revision;
        }
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult RenderSystem::publish(SceneStageContext&) noexcept
    {
        auto& self = *impl_;
        auto* frame = self.services.inputs.frame;
        if (!frame)
        {
            return ESceneProgress::PENDING;
        }
        if (self.state->busy)
        {
            return ESceneProgress::COMPLETE;
        }
        auto& client = self.services.rendering.mainClient();
        if (!self.transform_ids.empty())
        {
            auto matrices = frame->allocate<TransformMatrix>(self.transform_entities.size());
            for (std::size_t i = 0; i < matrices.size(); ++i)
            {
                const auto& world = self.registry.get<const WorldTransform3D>(self.transform_entities[i]);
                Eigen::Map<Eigen::Matrix4f>{matrices[i].data()} = world.value.matrix().cast<float>();
            }
            const TransformBatch batch{
                self.state->instance_storage,
                ++self.sequence,
                frame->freeze(std::span(self.transform_ids)),
                matrices
            };
            if (auto appended = client.append(*frame, *self.transforms, batch); !appended)
            {
                return failure(appended.error());
            }
        }
        if (!self.light_updates.empty())
        {
            UpdateDirectLights::Request request{
                self.state->light_storage,
                self.sequence,
                frame->freeze(std::span(self.light_updates))
            };
            if (auto appended = client.append(*frame, *self.lighting, request); !appended)
            {
                return failure(appended.error());
            }
        }
        if (self.parameters_changed && !self.state->attachment.isNull())
        {
            AttachmentInputs inputs{
                self.state->attachment,
                parameterBytes(self.state->parameters),
                self.services.inputs.viewport
            };
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
        static constexpr std::array requirements{
            SceneSystemRequirementSpec{"rendering", "lux.scene.rendering", cxx::typeToken<SceneRenderServices>(), false}
        };
        return {
            .type = system::systemTypeId(RenderSystem::Description.canonical_name),
            .cpp_type = cxx::typeToken<RenderSystem>(),
            .description = &RenderSystem::Description,
            .requirements = requirements,
            .install = +[](SceneSystemInstaller& builder,
                           SceneSystemDescription input) noexcept -> cxx::expected<void, SceneSystemBuildFailure>
            {
                auto* services = builder.require<SceneRenderServices>(input.instanceId(), "rendering");
                if (!services)
                {
                    return cxx::unexpected(SceneSystemBuildFailure{ESceneSystemBuildError::MISSING_REQUIREMENT});
                }
                auto instance = builder.emplaceSystem<RenderSystem>(input.instanceId(), builder.registry(), *services);
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
