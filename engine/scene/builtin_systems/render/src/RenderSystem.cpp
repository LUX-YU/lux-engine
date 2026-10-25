#include <lux/engine/scene/render/RasterAssetPipelines.hpp>
#include <lux/engine/render/capabilities/LitScene.hpp>
#include <lux/engine/render/capabilities/RasterAssets.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfigurationImpl.hpp>
#include <lux/engine/scene/RenderViewComponent.hpp>
#include <lux/engine/scene/SceneCamera.hpp>
#include <lux/engine/scene/SceneRenderInputs.hpp>

#include <exec/task.hpp>
#include <exec/variant_sender.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/inputs/DirectLightInputs.hpp>
#include <lux/engine/context/render/RenderAssets.hpp>
#include <lux/engine/context/render/RenderDiagnostics.hpp>
#include <lux/engine/context/render/RenderRequester.hpp>
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
    using namespace lux::render;
    using namespace engine::render;
    using namespace simulation::ecs;
    using detail::RenderViewBinding;
    using detail::RenderViewComponent;
    using namespace detail;

    namespace
    {
        bool sameTargetLayout(const TargetDesc& left, const TargetDesc& right) noexcept
        {
            // Built-in Scene recipes declare target-relative extents; dimensions belong to ResizeTarget.
            return left.kind == right.kind && left.format == right.format && left.samples == right.samples &&
                   left.sampled == right.sampled;
        }

        std::vector<std::byte> parameterBytes(const CameraInput& value)
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

        SceneStageResult failure(RenderError error) noexcept
        {
            return cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, error});
        }
    } // namespace

    struct RenderSystem::Impl final
    {
        struct CameraTargetWaiter
        {
            CameraTargetWaiter* next{};
            Entity camera;
            CameraTargetId target;
            bool retiring{};
            void (*complete)(CameraTargetWaiter&, RenderResult<void>) noexcept {};
            std::shared_ptr<const void> code_lifetime;
        };

        struct CameraTargetSender final
        {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(RenderResult<void>)>;
            Impl& system;
            Entity camera;
            CameraTargetId target;
            bool retiring;

            template <class Receiver> struct Operation final : CameraTargetWaiter
            {
                using operation_state_concept = stdexec::operation_state_t;
                Impl& system;
                Receiver receiver;

                Operation(const CameraTargetSender& sender, Receiver receiver) noexcept
                    : system(sender.system), receiver(std::move(receiver))
                {
                    this->camera = sender.camera;
                    this->target = sender.target;
                    this->retiring = sender.retiring;
                    this->code_lifetime = execution::detail::getCompletionCode(stdexec::get_env(this->receiver));
                    this->complete = [](CameraTargetWaiter& waiter, RenderResult<void> result) noexcept
                    {
                        auto& operation = static_cast<Operation&>(waiter);
                        stdexec::set_value(std::move(operation.receiver), std::move(result));
                    };
                }

                void start() & noexcept
                {
                    system.awaitCameraTarget(*this);
                }
            };

            template <class Receiver> auto connect(Receiver receiver) const noexcept
            {
                return Operation<Receiver>{*this, std::move(receiver)};
            }
        };

        bool cameraTargetRetired(Entity camera, CameraTargetId id) const noexcept
        {
            const bool active = changing_camera_target == std::pair{camera, id} ||
                                retiring_camera_target == std::pair{camera, id} ||
                                (!closing && cameraTarget(camera, id)) || cameraBinding(camera, id);
            return !active &&
                   std::ranges::none_of(
                       retiring_views,
                       [&](const auto& value) noexcept { return value.camera == camera && value.binding.id == id; }
                   );
        }

        std::optional<RenderResult<void>> cameraTargetCompletion(const CameraTargetWaiter& waiter) const noexcept
        {
            if (waiter.retiring && cameraTargetRetired(waiter.camera, waiter.target))
            {
                return RenderResult<void>{};
            }
            if (error.type != 0 && (!waiter.retiring || (closing && !transaction_pending && !retirement_pending)))
            {
                return RenderResult<void>{cxx::unexpected(error)};
            }
            if (!waiter.retiring)
            {
                if (closing)
                {
                    return RenderResult<void>{cxx::unexpected(RenderError{kCommandCancelled})};
                }
                const auto* desired = cameraTarget(waiter.camera, waiter.target);
                const auto* binding = cameraBinding(waiter.camera, waiter.target);
                if (!desired)
                {
                    return RenderResult<void>{cxx::unexpected(RenderError{kRuntimeIdentity})};
                }
                if (binding && binding->error.type != 0)
                {
                    return RenderResult<void>{cxx::unexpected(binding->error)};
                }
                if (binding && !binding->attachment.isNull() && binding->adopted.revision == desired->revision)
                {
                    return RenderResult<void>{};
                }
            }
            return {};
        }

        static void deliverCameraTarget(CameraTargetWaiter& waiter, RenderResult<void> result) noexcept
        {
            auto code_lifetime = std::move(waiter.code_lifetime);
            waiter.complete(waiter, std::move(result));
        }

        void awaitCameraTarget(CameraTargetWaiter& waiter) noexcept
        {
            if (auto ready = cameraTargetCompletion(waiter))
            {
                deliverCameraTarget(waiter, std::move(*ready));
                return;
            }
            waiter.next = std::exchange(camera_target_waiters, &waiter);
        }

        void completeCameraTargets() noexcept
        {
            auto** link = &camera_target_waiters;
            while (*link)
            {
                auto* waiter = *link;
                auto result = cameraTargetCompletion(*waiter);
                if (!result)
                {
                    link = &waiter->next;
                    continue;
                }
                *link = waiter->next;
                deliverCameraTarget(*waiter, std::move(*result));
            }
        }

        RenderResult<void> bindInputProducers(const CapabilitySupport& support) noexcept
        {
            const auto capability = std::ranges::find_if(
                support.capabilities,
                [this](const auto& value) noexcept
                { return value.name == configuration.capability && value.version == configuration.capability_version; }
            );
            for (const auto& input : capability->inputs)
            {
                const bool supported = supportsSceneRenderInput(input);
                if (!supported)
                {
                    if (input.required)
                    {
                        return cxx::unexpected(RenderError{kRuntimeUnsupported});
                    }
                    continue;
                }
                material_input_enabled |= input.schema == kRasterMaterialInput;
                instance_input_enabled |= input.schema == kRasterInstancesInput;
                light_input_enabled |= input.schema == kDirectLightsInput;
                if (input.schema == kSceneGeometryInput)
                {
                    effect_input_producer = &detail::produceLitSceneInputs;
                }
                camera_input_enabled |= input.schema == kCameraInput;
            }
            // Mesh3D projects a material reference with each instance. This Scene producer cannot
            // satisfy an instance-only contract by inventing a material destination.
            const bool missing_material_producer = instance_input_enabled && !material_input_enabled;
            if (missing_material_producer)
            {
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
            if (instance_input_enabled)
            {
                auto bound = client.bind<UpdateTransforms>();
                if (!bound)
                {
                    return cxx::unexpected(bound.error());
                }
                transforms = *bound;
            }
            if (light_input_enabled)
            {
                auto bound = client.bind<UpdateDirectLights>();
                if (!bound)
                {
                    return cxx::unexpected(bound.error());
                }
                lighting = *bound;
            }
            auto inputs_command = client.bind<UpdateAttachmentInputs>();
            auto active_command = client.bind<SetTargetActive>();
            if (!inputs_command || !active_command)
            {
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
            attachment_inputs = *inputs_command;
            target_active = *active_command;
            return {};
        }

        exec::task<RenderResult<void>> initialize() noexcept
        {
            EmptyRequest empty;
            auto created = co_await request<CreateScene>(client, empty);
            if (created.error.type != 0)
            {
                co_return cxx::unexpected(created.error);
            }
            scene = created.scene;
            CapabilityQuery query{
                configuration.capability,
                configuration.capability_version,
                configuration.implementation
            };
            const auto choices = co_await request<ListCapabilities>(client, query);
            if (choices.error.type != 0)
            {
                co_return cxx::unexpected(choices.error);
            }
            if (choices.implementations.size() != 1 || choices.implementations.front().unavailable.type != 0 ||
                choices.implementations.front().implementation_version != configuration.implementation_version)
            {
                co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
            auto producers = bindInputProducers(choices.implementations.front());
            if (!producers)
            {
                co_return producers;
            }
            implementation = choices.implementations.front().selection;
            CapabilityInstallation install{implementation, scene, configuration.configuration};
            if (instance_input_enabled || material_input_enabled)
            {
                auto prepared = co_await installRasterAssetPipelines(
                    assets, assets.resourceCapability()
                );
                if (!prepared)
                {
                    co_return prepared;
                }
            }
            const auto asset_inputs = assets.inputs();
            install.source_inputs.assign(asset_inputs.begin(), asset_inputs.end());
            const auto installed = co_await request<InstallCapabilities>(client, install);
            if (installed.error.type != 0)
            {
                co_return cxx::unexpected(installed.error);
            }
            for (const auto& input : installed.inputs)
            {
                if (input.schema == kRasterInstancesInput)
                {
                    instance_input = input.destination;
                }
                else if (input.schema == kRasterMaterialInput)
                {
                    material_input = input.destination;
                }
                else if (input.schema == kDirectLightsInput)
                {
                    light_input = input.destination;
                }
            }
            const bool missing_input = (instance_input_enabled && instance_input.implementation.isNull()) ||
                                       (material_input_enabled && material_input.implementation.isNull()) ||
                                       (light_input_enabled && light_input.implementation.isNull());
            if (missing_input)
            {
                co_return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
            initialized = true;
            co_return RenderResult<void>{};
        }

        RenderAssetVersion assetKey(asset::AssetId id, std::uint64_t revision) const noexcept
        {
            return {id, revision, assets_input.source};
        }

        void releaseBinding(const MeshBinding& binding) noexcept
        {
            assets.release<MeshAssetPipeline>(assetKey(binding.visual.mesh, binding.revision));
            --materials.find(assetKey(binding.visual.material, binding.revision))->second.references;
        }

        RenderResult<void> acquireBinding(const MeshBinding& binding) noexcept
        {
            const auto mesh_key = assetKey(binding.visual.mesh, binding.revision);
            const auto material_key = assetKey(binding.visual.material, binding.revision);
            auto acquired_mesh = assets.acquire<MeshAssetPipeline>(mesh_key, source, assets_input.snapshot);
            if (!acquired_mesh)
            {
                return acquired_mesh;
            }
            auto material = materials.find(material_key);
            if (material == materials.end())
            {
                auto acquired = assets.acquire<MaterialAssetPipeline>(material_key, source, assets_input.snapshot);
                if (!acquired)
                {
                    assets.release<MeshAssetPipeline>(mesh_key);
                    return acquired;
                }
                material = materials.emplace(material_key, MaterialEntry{.snapshot = assets_input.snapshot}).first;
                pending_materials.insert(material_key);
                ready_materials.insert(material_key);
            }
            ++material->second.references;
            asset_availability.observe<MeshAssetPipeline>(mesh_key);
            asset_availability.observe<MaterialAssetPipeline>(material_key);
            return {};
        }

        // These indices contain only outstanding demand, never live asset ownership. Delivery records
        // a version; the Main maintenance point selects the exact waiters before starting another task.
        void awaitBinding(std::uint64_t entity, const MeshBinding& binding) noexcept
        {
            pending_instances.insert(entity);
            ready_instances.insert(entity);
            instance_demands[assetKey(binding.visual.mesh, binding.revision)].insert(entity);
            instance_demands[assetKey(binding.visual.material, binding.revision)].insert(entity);
        }

        void finishBinding(std::uint64_t entity, const MeshBinding& binding) noexcept
        {
            for (const auto asset : {binding.visual.mesh, binding.visual.material})
            {
                const auto found = instance_demands.find(assetKey(asset, binding.revision));
                if (found != instance_demands.end())
                {
                    found->second.erase(entity);
                    if (found->second.empty())
                    {
                        instance_demands.erase(found);
                    }
                }
            }
            pending_instances.erase(entity);
            ready_instances.erase(entity);
        }

        void readyBindings(RenderAssetVersion key) noexcept
        {
            const auto found = instance_demands.find(key);
            if (found != instance_demands.end())
            {
                ready_instances.insert(found->second.begin(), found->second.end());
            }
        }

        void finishMaterial(RenderAssetVersion key, const MaterialEntry& entry) noexcept
        {
            for (const auto texture : entry.textures)
            {
                const auto found = material_demands.find(texture);
                if (found != material_demands.end())
                {
                    found->second.erase(key);
                    if (found->second.empty())
                    {
                        material_demands.erase(found);
                    }
                }
            }
            pending_materials.erase(key);
            ready_materials.erase(key);
            blocked_materials.erase(key);
            readyBindings(key);
        }

        void applyAssetCompletions() noexcept
        {
            for (const auto version : completed_assets)
            {
                if (pending_materials.contains(version))
                {
                    ready_materials.insert(version);
                }
                if (const auto found = material_demands.find(version); found != material_demands.end())
                {
                    ready_materials.insert(found->second.begin(), found->second.end());
                }
                readyBindings(version);
            }
            completed_assets.clear();
        }

        exec::task<void> prepareMaterials() noexcept
        {
            const auto ready = std::exchange(ready_materials, {});
            for (const auto key : ready)
            {
                auto& entry = materials.find(key)->second;
                const auto reject = [&](RenderError failure) noexcept
                {
                    entry.error = failure;
                    entry.snapshot = {};
                    finishMaterial(key, entry);
                    reportRenderFailure(failure, "Prepare Scene material candidate");
                };
                if (entry.references == 0 || !entry.material.isNull())
                {
                    continue;
                }
                const auto* cooked = cookedMaterial(assets, key);
                if (!cooked)
                {
                    const auto error = assets.failure<MaterialAssetPipeline>(key);
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
                        auto acquired = assets.acquire<TextureAssetPipeline>(texture, source, entry.snapshot);
                        if (!acquired && acquired.error().type == kRuntimeCapacity)
                        {
                            blocked_materials.insert(key);
                            asset_availability.whenCapacityAvailable();
                            break;
                        }
                        if (!acquired)
                        {
                            reject(acquired.error());
                            break;
                        }
                    }
                    entry.textures.push_back(texture);
                    if (!texture.asset.isNull())
                    {
                        material_demands[texture].insert(key);
                        asset_availability.observe<TextureAssetPipeline>(texture);
                    }
                }
                if (entry.textures.size() != cooked->texture_slot_ids.size())
                {
                    continue;
                }
                MaterialWrite input{material_input, cooked->programs, cooked->programs.parameter_defaults};
                bool ready = true;
                for (auto texture : entry.textures)
                {
                    if (texture.asset.isNull())
                    {
                        input.textures.push_back({});
                        continue;
                    }
                    const auto* uploaded = assets.ready<TextureAssetPipeline>(texture);
                    if (!uploaded)
                    {
                        const auto error = assets.failure<TextureAssetPipeline>(texture);
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
                finishMaterial(key, entry);
            }
            co_return;
        }

        const CameraTarget* cameraTarget(Entity camera, CameraTargetId id) const noexcept
        {
            const auto* outputs = registry.valid(camera) ? registry.try_get<CameraTargetsComponent>(camera) : nullptr;
            if (!outputs)
            {
                return nullptr;
            }
            const auto found = std::ranges::find(outputs->targets, id, &CameraTarget::id);
            return found == outputs->targets.end() ? nullptr : &*found;
        }

        RenderViewBinding* cameraBinding(Entity camera, CameraTargetId id) const noexcept
        {
            auto* views = registry.valid(camera) ? registry.try_get<RenderViewComponent>(camera) : nullptr;
            if (!views)
            {
                return nullptr;
            }
            const auto found = std::ranges::find(views->bindings, id, &RenderViewBinding::id);
            return found == views->bindings.end() ? nullptr : &*found;
        }

        RenderResult<CameraInput> cameraParameters(Entity camera, const CameraTarget& target) const noexcept
        {
            const bool available = registry.valid(camera) && registry.all_of<Camera, WorldTransform3D>(camera);
            if (!available)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            if (!camera_input_enabled && !effect_input_producer)
            {
                return CameraInput{};
            }
            const auto& definition = registry.get<Camera>(camera);
            auto prepared = detail::prepareSceneCamera(definition, registry.get<WorldTransform3D>(camera).value);
            return prepared ? detail::sceneParameters(definition, *prepared, target)
                            : cxx::unexpected(prepared.error());
        }

        template <class F> void forEachBinding(F&& function) const
        {
            for (const auto camera : registry.view<RenderViewComponent>())
            {
                for (auto& binding : registry.get<RenderViewComponent>(camera).bindings)
                {
                    function(camera, binding);
                }
            }
        }

        bool hasViews() const noexcept
        {
            return !registry.view<RenderViewComponent>().empty() || !retiring_views.empty();
        }

        // EnTT still exposes the component here. Only transfer records; never mutate the Registry in a signal.
        void bindingsDestroyed(Registry& registry, Entity camera) noexcept
        {
            for (auto& binding : registry.get<RenderViewComponent>(camera).bindings)
            {
                binding.adopted.active = false;
                binding.output_changed = true;
                retiring_views.push_back({camera, std::move(binding)});
            }
        }

        void reconcileCameraTargets() noexcept
        {
            for (const auto camera : view_changes)
            {
                if (!registry.valid(camera))
                {
                    continue;
                }
                const auto* desired = registry.try_get<CameraTargetsComponent>(camera);
                const bool available =
                    desired && !desired->targets.empty() && registry.all_of<Camera, WorldTransform3D>(camera);
                if (!available)
                {
                    registry.remove<RenderViewComponent>(camera);
                    continue;
                }
                auto& bindings = registry.get_or_emplace<RenderViewComponent>(camera).bindings;
                for (auto entry = bindings.begin(); entry != bindings.end();)
                {
                    if (std::ranges::find(desired->targets, entry->id, &CameraTarget::id) != desired->targets.end())
                    {
                        ++entry;
                        continue;
                    }
                    entry->adopted.active = false;
                    entry->output_changed = true;
                    retiring_views.push_back({camera, std::move(*entry)});
                    entry = bindings.erase(entry);
                }
                for (const auto& target : desired->targets)
                {
                    if (std::ranges::find(bindings, target.id, &RenderViewBinding::id) == bindings.end())
                    {
                        bindings.push_back(RenderViewBinding{.id = target.id});
                    }
                }
            }
        }

        exec::task<RenderResult<void>> releaseEffect(RenderViewBinding& binding) noexcept
        {
            if (!binding.effect.isNull())
            {
                EffectRequest remove{binding.effect};
                const auto result = co_await request<RemoveEffect>(client, remove);

                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
                binding.effect = {};
            }
            co_return RenderResult<void>{};
        }

        exec::task<RenderResult<void>> withdrawView(RenderViewBinding& binding) noexcept
        {
            // Stop future parameter publications before enqueuing removal on the same Main link.
            if (!binding.view.isNull())
            {
                ViewRequest remove{scene, binding.view};
                const auto result = co_await request<RemoveView>(client, remove);

                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
                binding.view = {};
            }
            binding.attachment = {};
            co_return co_await releaseEffect(binding);
        }

        exec::task<RenderResult<void>> retireViews() noexcept
        {
            while (!retiring_views.empty())
            {
                // A signal can append while a request is suspended. The coroutine owns this one exit record.
                const auto candidate = std::ranges::find_if(
                    retiring_views,
                    [this](const auto& value) noexcept
                    { return changing_camera_target != std::pair{value.camera, value.binding.id}; }
                );
                if (candidate == retiring_views.end())
                {
                    break;
                }
                auto retired = std::move(*candidate);
                retiring_views.erase(candidate);
                retiring_camera_target = std::pair{retired.camera, retired.binding.id};
                cxx::scope_exit finish([this]() noexcept { retiring_camera_target.reset(); });

                auto result = co_await withdrawView(retired.binding);

                if (!result)
                {
                    retiring_views.push_back(std::move(retired));
                    co_return result;
                }
            }
            co_return RenderResult<void>{};
        }

        detail::SceneEffectInputs prepareEffectInputs(bool include_replacements) noexcept
        {
            if (!effect_input_producer)
            {
                return {};
            }
            auto inputs = detail::prepareSceneEffectInputs(
                meshes,
                materials,
                lights,
                registry,
                assets,
                assets_input.source,
                effect_input_capacity,
                include_replacements
            );
            std::vector<RasterGeometrySelection> selection;
            selection.reserve(inputs.geometry.size());
            for (const auto& group : inputs.geometry)
            {
                selection.push_back(group.selection);
            }
            const bool changed_input = effect_geometry != selection || effect_lights != inputs.lights ||
                                       effect_input_capacity != inputs.capacity;
            if (changed_input)
            {
                effect_geometry = std::move(selection);
                effect_lights = inputs.lights;
                effect_input_capacity = inputs.capacity;
                ++effect_input_revision;
            }
            return inputs;
        }

        exec::task<RenderResult<void>> synchronizeViews(
            bool include_replacements = false,
            bool* inputs_adopted = nullptr,
            std::optional<std::vector<Entity>> selected_cameras = {}
        ) noexcept
        {
            const auto accepted_topology = topology_revision;
            const auto inputs = prepareEffectInputs(include_replacements);
            const auto accepted_input_revision = effect_input_revision;
            std::vector<std::pair<Entity, CameraTarget>> accepted;
            const auto accept = [&](Entity camera, const RenderViewBinding& binding) noexcept
            {
                if (const auto* target = cameraTarget(camera, binding.id))
                {
                    accepted.emplace_back(camera, *target);
                }
            };
            if (selected_cameras)
            {
                for (const auto camera : *selected_cameras)
                {
                    auto* views = registry.valid(camera) ? registry.try_get<RenderViewComponent>(camera) : nullptr;
                    if (views)
                    {
                        for (const auto& binding : views->bindings)
                        {
                            accept(camera, binding);
                        }
                    }
                }
            }
            else
            {
                forEachBinding(accept);
            }
            for (const auto& [camera, input] : accepted)
            {
                if (retiring_camera_target == std::pair{camera, input.id})
                {
                    // This relation's old handles are still being revoked. Other relations may advance;
                    // its replacement resumes after retirement, without sharing the suspended exit record.
                    view_changes.insert(camera);
                    continue;
                }
                const auto* current = cameraTarget(camera, input.id);
                auto* binding = cameraBinding(camera, input.id);
                if (!current || current->revision != input.revision || !binding)
                {
                    continue;
                }
                changing_camera_target = std::pair{camera, input.id};
                cxx::scope_exit finish([this]() noexcept { changing_camera_target.reset(); });
                auto parameters = cameraParameters(camera, input);
                if (!parameters)
                {
                    const auto id = binding->id;
                    retiring_views.push_back({camera, std::move(*binding)});
                    *binding = RenderViewBinding{.id = id, .error = parameters.error()};
                    continue;
                }
                const auto key =
                    detail::cameraGraphInputs(registry.get<Camera>(camera), input, effect_input_producer != nullptr);
                const bool same_inputs = !binding->attachment.isNull() && binding->graph_inputs == key &&
                                         binding->input_revision == accepted_input_revision;
                if (same_inputs)
                {
                    binding->parameters_changed |= !detail::sameParameters(binding->parameters, *parameters) ||
                                                   binding->adopted.viewport != input.viewport;
                    binding->output_changed |= binding->adopted.description != input.description ||
                                               binding->adopted.active != input.active ||
                                               binding->adopted.epoch != input.epoch;
                    binding->parameters = *parameters;
                    binding->adopted = input;
                    binding->error = {};
                    continue;
                }
                if (binding->view.isNull())
                {
                    CreateViewRequest create{scene};
                    const auto result = co_await request<CreateView>(client, create);
                    if (result.error.type != 0)
                    {
                        co_return cxx::unexpected(result.error);
                    }
                    binding = cameraBinding(camera, input.id);
                    if (!binding)
                    {
                        retiring_views.push_back({camera, RenderViewBinding{.id = input.id, .view = result.view}});
                        continue;
                    }
                    binding->view = result.view;
                }
                const auto accepted_view = binding->view;
                auto data = effect_input_producer ? effect_input_producer(inputs, *parameters)
                                                  : RenderResult<ConfigValue>{ConfigValue{}};
                if (!data)
                {
                    co_return cxx::unexpected(data.error());
                }
                CreateEffectRequest
                    create_effect{input.description, implementation, configuration.configuration, std::move(*data)};

                const auto compiled = co_await request<CreateEffect>(client, create_effect);
                if (compiled.error.type != 0)
                {
                    co_return cxx::unexpected(compiled.error);
                }
                // No component address or vector iterator survives a suspension. A deleted relation never reappears.
                current = cameraTarget(camera, input.id);
                binding = cameraBinding(camera, input.id);
                auto latest_parameters = cameraParameters(camera, input);
                const bool valid_camera = registry.valid(camera) && registry.all_of<Camera>(camera);
                const bool changed_graph_inputs =
                    !valid_camera ||
                    detail::cameraGraphInputs(registry.get<Camera>(camera), input, effect_input_producer != nullptr) !=
                        key;
                const bool stale = !current || current->revision != input.revision || !binding || !latest_parameters ||
                                   binding->view != accepted_view || accepted_topology != topology_revision ||
                                   changed_graph_inputs || closing;
                if (stale)
                {
                    retiring_views.push_back({camera, RenderViewBinding{.id = input.id, .effect = compiled.effect}});
                    rebuild_requested |= accepted_topology != topology_revision;
                    continue;
                }
                AttachmentRequest attach{
                    scene,
                    accepted_view,
                    input.target,
                    compiled.effect,
                    camera_input_enabled ? parameterBytes(*latest_parameters) : std::vector<std::byte>{},
                    input.viewport,
                    {},
                    binding->attachment
                };
                attach.parameter_type = camera_input_enabled ? cxx::typeToken<CameraInput>() : cxx::TypeToken{};
                const auto adopted = co_await request<Attach>(client, attach);
                binding = cameraBinding(camera, input.id);
                current = cameraTarget(camera, input.id);
                auto adopted_parameters = cameraParameters(camera, input);
                const bool still_available =
                    current && binding && registry.valid(camera) && registry.all_of<Camera>(camera);
                const bool obsolete =
                    !still_available || !adopted_parameters || closing || binding->view != accepted_view ||
                    current->revision != input.revision || accepted_topology != topology_revision ||
                    detail::cameraGraphInputs(registry.get<Camera>(camera), input, effect_input_producer != nullptr) !=
                        key;
                if (adopted.error.type != 0 || obsolete)
                {
                    retiring_views.push_back({camera, RenderViewBinding{.id = input.id, .effect = compiled.effect}});
                    if (adopted.error.type == 0)
                    {
                        // Attach really replaced the old relation. Withdraw the accepted candidate before retrying;
                        // never publish its identity with parameters from a different cold recipe.
                        AttachmentId detach = adopted.attachment;
                        const auto withdrawn = co_await request<Detach>(client, detach);
                        if (withdrawn.error.type != 0)
                        {
                            co_return cxx::unexpected(withdrawn.error);
                        }
                        binding = cameraBinding(camera, input.id);
                        if (binding)
                        {
                            binding->attachment = {};
                        }
                    }
                    if (adopted.error.type != 0)
                    {
                        co_return cxx::unexpected(adopted.error);
                    }
                    continue;
                }
                if (!binding->effect.isNull())
                {
                    retiring_views.push_back({camera, RenderViewBinding{.id = input.id, .effect = binding->effect}});
                }
                binding->effect = compiled.effect;
                binding->attachment = adopted.attachment;
                binding->adopted = input;
                binding->graph_inputs = key;
                binding->parameters = *adopted_parameters;
                binding->parameters_changed = true;
                binding->output_changed = true;
                binding->input_revision = accepted_input_revision;
                binding->error = {};
            }
            if (inputs_adopted)
            {
                *inputs_adopted = true;
                forEachBinding(
                    [&](Entity, const RenderViewBinding& binding) noexcept
                    {
                        *inputs_adopted &=
                            binding.attachment.isNull() || binding.input_revision == accepted_input_revision;
                    }
                );
            }
            co_return RenderResult<void>{};
        }

        exec::task<RenderResult<void>> synchronizeStructure(
            std::vector<InstanceInput> inputs,
            std::vector<LightInput> light_inputs,
            bool structure_changed
        ) noexcept
        {
            if (closing)
            {
                co_return RenderResult<void>{};
            }
            bool changed = rebuild_requested;
            bool previous_inputs_removed = false;
            if (structure_changed)
            {
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
                CreateDirectLights::Request create{light_input, 0, values};
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
                if (!detail::bindingReady(binding, assets, materials, assets_input.source, entry.error))
                {
                    if (entry.error.type != 0)
                    {
                        // Failed candidates remain diagnosable; a new author/asset revision retries them.
                        finishBinding(input.entity, binding);
                    }
                    continue;
                }
                const auto* mesh = assets.ready<MeshAssetPipeline>(assetKey(binding.visual.mesh, binding.revision));
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
                MeshInstancesCreate create{instance_input, descriptions};
                auto created = co_await request<CreateMeshInstances>(client, create);
                if (created.error.type != 0)
                {
                    for (const auto entity : entities)
                    {
                        meshes.find(entity)->second.error = created.error;
                        const auto& entry = meshes.find(entity)->second;
                        finishBinding(entity, entry.replacement ? *entry.replacement : entry.binding);
                    }
                    reportRenderFailure(created.error, "Create Scene instance candidates");
                }
                else
                {
                    for (std::size_t i = 0; i < entities.size(); ++i)
                    {
                        auto& entry = meshes.find(entities[i])->second;
                        entry.instance = created.instances[i];
                        finishBinding(entities[i], entry.replacement ? *entry.replacement : entry.binding);
                        if (entry.replacement)
                        {
                            releaseBinding(entry.binding);
                            entry.binding = std::move(*entry.replacement);
                            entry.replacement.reset();
                        }
                        new_instances.push_back(entities[i]);
                    }
                }
                changed = true;
            }
            if ((changed || !replacements.empty()) && !closing)
            {
                const auto accepted_views = camera_revision;
                const auto accepted_topology = topology_revision;
                // A temporary recipe accepts both old and candidate groups. It changes no instance:
                // every observation can keep drawing the old binding until the single frame update.
                bool inputs_adopted = false;
                auto rebuilt = co_await synchronizeViews(!replacements.empty(), &inputs_adopted);
                if (!rebuilt)
                {
                    co_return rebuilt;
                }
                const bool changed_during_compile =
                    !inputs_adopted || accepted_views != camera_revision || accepted_topology != topology_revision;
                rebuild_requested = changed_during_compile;
                previous_inputs_removed = replacements.empty() && inputs_adopted;
                if (!replacements.empty() && !changed_during_compile)
                {
                    MeshInstanceBatch update{instance_input, ++sequence, replacements};
                    const auto result = co_await request<UpdateMeshInstances>(client, update);
                    if (result.error.type == 0)
                    {
                        for (const auto entity : replacement_entities)
                        {
                            auto& entry = meshes.find(entity)->second;
                            finishBinding(entity, *entry.replacement);
                            entry.previous = std::move(entry.binding);
                            entry.binding = std::move(*entry.replacement);
                            entry.replacement.reset();
                            new_instances.push_back(entity);
                        }
                        auto pruned = co_await synchronizeViews(false, &previous_inputs_removed);
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
                            auto& entry = meshes.find(entity)->second;
                            finishBinding(entity, *entry.replacement);
                        }
                        reportRenderFailure(result.error, "Adopt Scene instance candidates");
                    }
                }
            }
            if (!structure_changed && !changed && replacements.empty())
            {
                co_return RenderResult<void>{};
            }
            rebuild_requested |= std::ranges::any_of(
                                     meshes,
                                     [](const auto& value) noexcept { return value.second.previous.has_value(); }
                                 ) &&
                                 !previous_inputs_removed;
            // All current recipes have stopped mentioning these resources. Native retirement still
            // governs actual GPU use; this only releases the Engine's asset reference.
            for (auto& [entity, entry] : meshes)
            {
                if (entry.previous && previous_inputs_removed)
                {
                    releaseBinding(*entry.previous);
                    entry.previous.reset();
                }
            }
            for (auto entry = meshes.begin(); entry != meshes.end();)
            {
                if (entry->second.present || !previous_inputs_removed)
                {
                    ++entry;
                    continue;
                }
                releaseBinding(entry->second.binding);
                if (entry->second.replacement)
                {
                    releaseBinding(*entry->second.replacement);
                }
                finishBinding(
                    entry->first,
                    entry->second.replacement ? *entry->second.replacement : entry->second.binding
                );
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
                        assets.release<TextureAssetPipeline>(texture);
                    }
                }
                assets.release<MaterialAssetPipeline>(entry->first);
                finishMaterial(entry->first, entry->second);
                entry = materials.erase(entry);
            }
            co_return RenderResult<void>{};
        }

        exec::task<RenderResult<void>> closeScene() noexcept
        {
            if (client.serverStopped())
            {
                // The joined Server has discharged all native objects. Only Client reference pairs remain.
                scene = {};
                forEachBinding(
                    [](Entity, RenderViewBinding& binding) noexcept
                    {
                        binding.view = {};
                        binding.attachment = {};
                        binding.effect = {};
                    }
                );
                for (auto& retired : retiring_views)
                {
                    retired.binding.view = {};
                    retired.binding.attachment = {};
                    retired.binding.effect = {};
                }
                for (auto& [key, material] : materials)
                {
                    material.material = {};
                }
            }
            if (!scene.isNull())
            {
                SceneRequest remove{scene};
                auto result = co_await request<RemoveScene>(client, remove);
                if (result.error.type != 0)
                {
                    co_return cxx::unexpected(result.error);
                }
                scene = {};
                forEachBinding(
                    [](Entity, RenderViewBinding& binding) noexcept
                    {
                        binding.view = {};
                        binding.attachment = {};
                    }
                );
                for (auto& retired : retiring_views)
                {
                    retired.binding.view = {};
                    retired.binding.attachment = {};
                }
            }
            registry.clear<RenderViewComponent>();
            auto retired = co_await retireViews();
            if (!retired)
            {
                co_return retired;
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
                        assets.release<TextureAssetPipeline>(texture);
                    }
                }
                assets.release<MaterialAssetPipeline>(entry->first);
                materials.erase(entry);
            }
            pending_instances.clear();
            pending_materials.clear();
            ready_instances.clear();
            ready_materials.clear();
            blocked_materials.clear();
            completed_assets.clear();
            instance_demands.clear();
            material_demands.clear();
            lights.clear();
            co_return RenderResult<void>{};
        }

        void startViewRetirement() noexcept
        {
            const bool available = std::ranges::any_of(
                retiring_views,
                [this](const auto& value) noexcept
                { return changing_camera_target != std::pair{value.camera, value.binding.id}; }
            );
            if (retirement_pending || !available)
            {
                return;
            }
            retirement_pending = true;
            auto ready = execution::taskResult(stdexec::starts_on(execution.mainScheduler(), retireViews()));
            const auto accepted = execution.spawn(stdexec::then(
                std::move(ready),
                [this](auto result) noexcept
                {
                    retirement_pending = false;

                    if (!result)
                    {
                        const auto failure = result.error().domainFailure();
                        error = failure ? *failure : RenderError{kRuntimeState};
                        if (error.type == kDeviceBusy || error.type == kRuntimeCapacity)
                        {
                            error = {};
                        }
                        else
                        {
                            reportRenderFailure(error, "Retire Camera Target");
                        }
                    }
                    completeCameraTargets();
                }
            ));
            if (!accepted)
            {
                std::terminate(); // Shared execution remains open through Scene retirement.
            }
        }

        void startTransaction(exec::task<RenderResult<void>> work) noexcept
        {
            transaction_pending = true;
            auto ready = execution::taskResult(stdexec::starts_on(execution.mainScheduler(), std::move(work)));
            auto accepted = execution.spawn(stdexec::then(
                std::move(ready),
                [this](auto result) noexcept
                {
                    if (!result && error.type == 0)
                    {
                        error = result.error().domainFailure() ? *result.error().domainFailure()
                                                               : RenderError{kRuntimeState};
                    }
                    const bool retry = error.type == kDeviceBusy || error.type == kRuntimeCapacity;
                    if (retry)
                    {
                        rebuild_requested = true;
                        error = {}; // The next admitted Scene maintenance frame retries the unsettled records.
                    }
                    else if (error.type != 0)
                    {
                        reportRenderFailure(error, closing ? "Close Render Scene" : "Synchronize Render Scene");
                    }
                    transaction_pending = false;
                    completeCameraTargets();
                }
            ));
            if (!accepted)
            {
                transaction_pending = false;
                error = RenderError{kRuntimeState};
                reportRenderFailure(error, "Admit Render Scene operation");
                completeCameraTargets();
            }
        }

        RenderResult<bool> collectMeshChanges() noexcept
        {
            if (!instance_input_enabled)
            {
                structure.clear();
                return false;
            }
            if (asset_capacity_blocked)
            {
                return false;
            }
            asset_capacity_blocked = false;
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
                        asset_capacity_blocked = true;
                        asset_availability.whenCapacityAvailable();
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
                    finishBinding(bits, entry.replacement ? *entry.replacement : entry.binding);
                    if (entry.replacement)
                    {
                        releaseBinding(*entry.replacement);
                    }
                    entry.replacement = std::move(candidate);
                    entry.error = {};
                }
                const auto& entry = meshes.find(bits)->second;
                awaitBinding(bits, entry.replacement ? *entry.replacement : entry.binding);
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
            if (!light_input_enabled)
            {
                light_changes.clear();
                return false;
            }
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
            if (!retiring_views.empty())
            {
                return true;
            }
            for (const auto camera : view_changes)
            {
                const auto* views = registry.valid(camera) ? registry.try_get<RenderViewComponent>(camera) : nullptr;
                if (!views)
                {
                    continue;
                }
                for (const auto& binding : views->bindings)
                {
                    const auto* target = cameraTarget(camera, binding.id);
                    if (!target)
                    {
                        continue;
                    }
                    const auto key = detail::cameraGraphInputs(
                        registry.get<Camera>(camera),
                        *target,
                        effect_input_producer != nullptr
                    );
                    if (binding.graph_inputs != key || binding.attachment.isNull())
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        void prepareCameraParameters() noexcept
        {
            for (const auto camera : camera_changes)
            {
                auto* views = registry.valid(camera) ? registry.try_get<RenderViewComponent>(camera) : nullptr;
                if (!views)
                {
                    continue;
                }
                const bool available = registry.all_of<Camera, WorldTransform3D>(camera);
                auto prepared = available && camera_input_enabled
                                    ? detail::prepareSceneCamera(
                                          registry.get<Camera>(camera),
                                          registry.get<WorldTransform3D>(camera).value
                                      )
                                    : RenderResult<detail::SceneCamera>{cxx::unexpected(RenderError{kCommandContract})};
                for (auto& binding : views->bindings)
                {
                    const auto* input = cameraTarget(camera, binding.id);
                    if (!input || binding.attachment.isNull())
                    {
                        continue;
                    }
                    const bool graph_inputs_changed = available && detail::cameraGraphInputs(
                                                                       registry.get<Camera>(camera),
                                                                       *input,
                                                                       effect_input_producer != nullptr
                                                                   ) != binding.graph_inputs;
                    if (graph_inputs_changed)
                    {
                        continue; // Keep the complete last-good parameter set until its replacement recipe adopts.
                    }
                    const bool same_output = binding.adopted.target == input->target &&
                                             sameTargetLayout(binding.adopted.description, input->description);
                    const auto& requested = same_output ? *input : binding.adopted;
                    auto candidate = !camera_input_enabled ? RenderResult<CameraInput>{CameraInput{}}
                                     : prepared
                                         ? detail::sceneParameters(registry.get<Camera>(camera), *prepared, requested)
                                         : RenderResult<CameraInput>{cxx::unexpected(prepared.error())};
                    if (!candidate)
                    {
                        binding.error = candidate.error();
                        binding.adopted.active = false;
                        binding.output_changed = true;
                        continue;
                    }
                    binding.error = {};
                    binding.parameters_changed |= !detail::sameParameters(binding.parameters, *candidate) ||
                                                  binding.adopted.viewport != requested.viewport;
                    binding.parameters = *candidate;
                    if (same_output)
                    {
                        binding.output_changed |= binding.adopted.description != requested.description ||
                                                  binding.adopted.active != requested.active ||
                                                  binding.adopted.epoch != requested.epoch;
                        binding.adopted = requested;
                    }
                }
            }
            camera_changes.clear();
        }

        Impl(Registry& registry, const RenderSystemDependencies& dependencies, RenderSystemConfiguration selected)
            : registry(registry), assets_input(dependencies.assets_input), client(dependencies.client),
              execution(dependencies.execution), assets(dependencies.assets),
              asset_availability(
                  assets,
                  this,
                  [](void* value, RenderAssetVersion version) noexcept
                  { static_cast<Impl*>(value)->completed_assets.insert(version); },
                  [](void* value) noexcept
                  {
                      auto& system = *static_cast<Impl*>(value);
                      system.asset_capacity_blocked = false;
                      system.ready_materials.insert(system.blocked_materials.begin(), system.blocked_materials.end());
                      system.blocked_materials.clear();
                  }
              ),
              source(dependencies.source), configuration(std::move(selected)),
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
              light_removed(registry.on_destroy<Light3D>().connect<&Impl::lightDescriptionChanged>(*this)),
              targets_added(registry.on_construct<CameraTargetsComponent>().connect<&Impl::cameraChanged>(*this)),
              targets_updated(registry.on_update<CameraTargetsComponent>().connect<&Impl::cameraChanged>(*this)),
              targets_removed(registry.on_destroy<CameraTargetsComponent>().connect<&Impl::cameraChanged>(*this)),
              bindings_removed(registry.on_destroy<RenderViewComponent>().connect<&Impl::bindingsDestroyed>(*this))
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
            for (const auto camera : registry.view<const CameraTargetsComponent>())
            {
                cameraChanged(registry, camera);
            }
        }

        ~Impl() noexcept
        {
            if (camera_target_waiters || transaction_pending || retirement_pending || !scene.isNull() || hasViews() ||
                !meshes.empty() || !materials.empty())
            {
                std::terminate(); // Retirement maintenance finishes before the System is destroyed.
            }
        }

        void structureChanged(Registry&, Entity entity) noexcept
        {
            asset_capacity_blocked = false; // A new author change may remove/reuse demand even before a load completes.
            if (!structure.contains(entity))
            {
                structure.insert(entity);
            }
        }

        void poseAdded(Registry& registry, Entity entity) noexcept
        {
            ++topology_revision;
            structureChanged(registry, entity);
            cameraChanged(registry, entity);
            transformChanged(registry, entity);
        }

        void cameraChanged(Registry&, Entity camera) noexcept
        {
            camera_changes.insert(camera);
            view_changes.insert(camera);
            ++camera_revision;
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
                cameraChanged(registry, entity);
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
                camera_changes.insert(entity);
            }
            if (!transformed.contains(entity))
            {
                transformed.insert(entity);
            }
        }

        Registry& registry;
        RenderAssetInput& assets_input;
        RenderRequester& client;
        execution::ExecutionRuntime& execution;
        RenderAssets& assets;
        RenderAssets::AvailabilityConnection asset_availability;
        process::asset_loading::AssetReadPort source;
        std::unordered_map<std::uint64_t, MeshEntry> meshes;
        std::vector<std::uint64_t> new_instances, new_lights;
        std::unordered_set<std::uint64_t> pending_instances;
        std::unordered_map<std::uint64_t, LightEntry> lights;
        std::map<RenderAssetVersion, MaterialEntry> materials;
        std::set<RenderAssetVersion> pending_materials, ready_materials, blocked_materials, completed_assets;
        std::unordered_set<std::uint64_t> ready_instances;
        std::map<RenderAssetVersion, std::unordered_set<std::uint64_t>> instance_demands;
        std::map<RenderAssetVersion, std::set<RenderAssetVersion>> material_demands;
        SceneId scene;
        std::vector<detail::RetiringView> retiring_views;
        std::uint64_t next_camera_target = 0, camera_revision = 0;
        CapabilityInput instance_input, material_input, light_input;
        CapabilitySelection implementation;
        RenderSystemConfiguration configuration;
        // Cold input identity is shared by this Scene's views. Bindings retain only the adopted revision.
        std::vector<RasterGeometrySelection> effect_geometry;
        std::vector<SceneLightInput> effect_lights;
        std::uint32_t effect_input_capacity = 128;
        std::uint64_t effect_input_revision = 0;
        bool material_input_enabled = false, instance_input_enabled = false, light_input_enabled = false;
        detail::EffectInputProducer effect_input_producer = nullptr;
        bool camera_input_enabled = false;
        RenderError error;
        bool initialized = false, transaction_pending = false, retirement_pending = false, closing = false;
        std::optional<std::pair<Entity, CameraTargetId>> changing_camera_target, retiring_camera_target;
        CameraTargetWaiter* camera_target_waiters{};
        bool rebuild_requested = false;

        // Dirty intents can contain an erased entity and its reused slot's next generation in
        // one Main interval (for example Undo + Redo). A live-component sparse set cannot.
        entt::dense_set<Entity> structure, transformed, light_changes, camera_changes, view_changes;
        std::vector<Entity> processed;
        std::vector<MeshInstanceId> transform_ids;
        std::vector<Entity> transform_entities;
        std::vector<DirectLightUpdate> light_updates;
        std::optional<BoundCommand<UpdateTransforms>> transforms;
        std::optional<BoundCommand<UpdateDirectLights>> lighting;
        std::optional<BoundCommand<UpdateAttachmentInputs>> attachment_inputs;
        std::optional<BoundCommand<SetTargetActive>> target_active;
        std::uint64_t sequence = 0, resynchronization = 0;
        bool asset_capacity_blocked{};
        std::uint64_t asset_revision = 0;
        std::uint64_t topology_revision = 0, observed_topology = UINT64_MAX;
        entt::scoped_connection mesh_added, mesh_updated, mesh_removed, pose_added, pose_updated, pose_removed;
        entt::scoped_connection camera_added, camera_updated, camera_removed;
        entt::scoped_connection light_added, light_updated, light_removed;
        entt::scoped_connection targets_added, targets_updated, targets_removed, bindings_removed;
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

    RenderResult<CameraTargetId> RenderSystem::addCameraTarget(Entity camera, CameraTarget target) noexcept
    {
        auto& self = *impl_;
        const bool available = self.registry.valid(camera) && self.registry.all_of<Camera>(camera);
        if (!available || self.closing || target.target.isNull() || !target.viewport.validate())
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        if (self.next_camera_target == UINT64_MAX)
        {
            std::terminate(); // Scene-local identities cannot wrap or be reused.
        }
        target.id = CameraTargetId{++self.next_camera_target};
        target.revision = 1;
        static_cast<void>(self.registry.get_or_emplace<CameraTargetsComponent>(camera));
        self.registry.patch<CameraTargetsComponent>(
            camera,
            [&](auto& component) { component.targets.push_back(target); }
        );
        return target.id;
    }

    bool RenderSystem::updateCameraTarget(Entity camera, CameraTargetId id, CameraTarget target) noexcept
    {
        auto& self = *impl_;
        const auto* current = self.cameraTarget(camera, id);
        if (!current || self.closing || target.target.isNull() || !target.viewport.validate())
        {
            return false;
        }
        target.id = id;
        target.revision = current->revision + 1;
        self.registry.patch<CameraTargetsComponent>(
            camera,
            [&](auto& component) { *std::ranges::find(component.targets, id, &CameraTarget::id) = target; }
        );
        return true;
    }

    bool RenderSystem::removeCameraTarget(Entity camera, CameraTargetId id) noexcept
    {
        auto& self = *impl_;
        if (!self.cameraTarget(camera, id))
        {
            return false;
        }
        self.registry.patch<CameraTargetsComponent>(
            camera,
            [&](auto& component)
            { std::erase_if(component.targets, [&](const auto& value) noexcept { return value.id == id; }); }
        );
        return true;
    }

    AttachmentId RenderSystem::attachment(Entity camera, CameraTargetId id) const noexcept
    {
        const auto* binding = impl_->cameraBinding(camera, id);
        return binding ? binding->attachment : AttachmentId{};
    }

    RenderError RenderSystem::cameraTargetError(Entity camera, CameraTargetId id) const noexcept
    {
        const auto* binding = impl_->cameraBinding(camera, id);
        if (binding)
        {
            return binding->error;
        }
        if (!impl_->cameraTarget(camera, id))
        {
            return {};
        }
        // A newly edited Transform can still be awaiting the existing TransformSystem's stable point.
        const bool has_pose = impl_->registry.any_of<Transform3D, WorldTransform3D>(camera);
        const bool missing_camera = !impl_->registry.all_of<Camera>(camera) || !has_pose;
        return missing_camera ? RenderError{kCommandContract} : RenderError{};
    }

    RenderError RenderSystem::assetError(Entity entity) const noexcept
    {
        const auto found = impl_->meshes.find(entityBits(entity));
        return found == impl_->meshes.end() ? RenderError{} : found->second.error;
    }

    bool RenderSystem::visualReady(Entity entity) const noexcept
    {
        const auto found = impl_->meshes.find(entityBits(entity));
        return found != impl_->meshes.end() && !found->second.instance.isNull() && found->second.error.type == 0 &&
               !found->second.replacement && !impl_->structure.contains(entity) &&
               !impl_->pending_instances.contains(entityBits(entity));
    }

    bool RenderSystem::cameraTargetRetired(Entity camera, CameraTargetId id) const noexcept
    {
        return impl_->cameraTargetRetired(camera, id);
    }

    exec::task<RenderResult<void>> RenderSystem::whenCameraTargetReady(Entity camera, CameraTargetId id) noexcept
    {
        co_return co_await stdexec::continues_on(
            Impl::CameraTargetSender{*impl_, camera, id, false},
            impl_->execution.mainScheduler()
        );
    }

    exec::task<RenderResult<void>> RenderSystem::whenCameraTargetRetired(Entity camera, CameraTargetId id) noexcept
    {
        co_return co_await stdexec::continues_on(
            Impl::CameraTargetSender{*impl_, camera, id, true},
            impl_->execution.mainScheduler()
        );
    }

    SceneStageResult RenderSystem::maintain(SceneStageContext& context) noexcept
    {
        auto& self = *impl_;
        if (context.stop.stop_requested())
        {
            if (self.transaction_pending || self.retirement_pending)
            {
                return ESceneProgress::PENDING;
            }
            if (!self.closing)
            {
                self.closing = true;
                self.error = {}; // Structural failure does not discharge its created handles.
            }
            if (self.client.serverStopped())
            {
                self.error = {}; // The SceneRuntime/App retains the failure; final physical cleanup is now proved.
            }
            if (self.error.type != 0 && self.error.type != kRuntimeCapacity)
            {
                return failure(self.error);
            }
            const bool has_resources =
                !self.scene.isNull() || self.hasViews() || !self.meshes.empty() || !self.materials.empty();
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
        const bool changed_inputs = !self.camera_changes.empty() || !self.view_changes.empty() ||
                                    self.resynchronization != self.client.resynchronization();
        context.publication_needed |= !self.structure.empty() || !self.transformed.empty() ||
                                      !self.light_changes.empty() || changed_inputs || self.transaction_pending;
        if (!context.allow_structure)
        {
            return ESceneProgress::COMPLETE;
        }
        self.reconcileCameraTargets();
        self.startViewRetirement();
        self.completeCameraTargets();
        if (self.transaction_pending)
        {
            return ESceneProgress::COMPLETE;
        }
        if (!self.initialized)
        {
            self.startTransaction(self.initialize());
            return ESceneProgress::PENDING;
        }
        const bool changed_views = self.needsViewSynchronization();
        self.rebuild_requested |= self.observed_topology != self.topology_revision;
        self.observed_topology = self.topology_revision;
        bool changed = !self.initialized || self.rebuild_requested;
        if (self.asset_revision != self.assets_input.revision)
        {
            self.asset_revision = self.assets_input.revision;
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
        self.applyAssetCompletions();
        if (changed || !self.ready_instances.empty() || !self.ready_materials.empty())
        {
            std::vector<InstanceInput> inputs;
            const auto ready_instances = std::exchange(self.ready_instances, {});
            for (const auto bits : ready_instances)
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
            if (changed)
            {
                self.view_changes.clear();
            }
            self.startTransaction(self.synchronizeStructure(std::move(inputs), std::move(light_inputs), changed));
        }
        else if (changed_views)
        {
            std::vector<Entity> selected(self.view_changes.begin(), self.view_changes.end());
            self.view_changes.clear();
            self.startTransaction(self.synchronizeViews(false, nullptr, std::move(selected)));
        }
        else
        {
            self.view_changes.clear();
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
            self.forEachBinding(
                [](Entity, RenderViewBinding& binding) noexcept
                {
                    binding.parameters_changed = true;
                    binding.output_changed = true;
                }
            );
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
            self.transform_ids.push_back(entry->second.instance);
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
        self.prepareCameraParameters();
        self.completeCameraTargets();
        return ESceneProgress::COMPLETE;
    }

    SceneStageResult RenderSystem::publish(SceneStageContext&) noexcept
    {
        auto& self = *impl_;
        if (self.closing)
        {
            return ESceneProgress::COMPLETE;
        }
        auto* frame = self.client.buildingFrame();
        if (!frame)
        {
            self.client.requestFrameInput();
            return ESceneProgress::PENDING;
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
                batch{self.instance_input, ++self.sequence, frame->freeze(std::span(self.transform_ids)), matrices};
            if (auto appended = client.append(*frame, *self.transforms, batch); !appended)
            {
                return failure(appended.error());
            }
        }
        if (!self.light_updates.empty())
        {
            UpdateDirectLights::Request request{
                self.light_input,
                self.sequence,
                frame->freeze(std::span(self.light_updates))
            };
            if (auto appended = client.append(*frame, *self.lighting, request); !appended)
            {
                return failure(appended.error());
            }
        }
        RenderError publication_error;
        const auto publishActivation = [&](RenderViewBinding& binding) noexcept
        {
            if (!binding.output_changed || publication_error.type != 0)
            {
                return;
            }
            const SetTargetActive::Request active{
                binding.adopted.target,
                binding.adopted.active,
                binding.adopted.epoch
            };
            if (auto appended = client.append(*frame, *self.target_active, active); !appended)
            {
                publication_error = appended.error();
                return;
            }
            binding.output_changed = false;
        };
        self.forEachBinding(
            [&](Entity camera, RenderViewBinding& binding) noexcept
            {
                const bool can_publish = self.cameraTarget(camera, binding.id) && !binding.attachment.isNull();
                if (!can_publish || publication_error.type != 0)
                {
                    return;
                }
                if (binding.error.type != 0)
                {
                    // Missing/invalid Camera data stops future draws, even while another recipe is compiling.
                    publishActivation(binding);
                    return;
                }
                if (binding.parameters_changed)
                {
                    AttachmentInputs inputs{
                        binding.attachment,
                        self.camera_input_enabled ? parameterBytes(binding.parameters) : std::vector<std::byte>{},
                        binding.adopted.viewport
                    };
                    inputs.parameter_type =
                        self.camera_input_enabled ? cxx::typeToken<CameraInput>() : cxx::TypeToken{};
                    if (auto appended = client.append(*frame, *self.attachment_inputs, std::move(inputs)); !appended)
                    {
                        publication_error = appended.error();
                        return;
                    }
                    binding.parameters_changed = false;
                }
                publishActivation(binding);
            }
        );
        for (auto& retired : self.retiring_views)
        {
            if (!retired.binding.attachment.isNull())
            {
                publishActivation(retired.binding);
            }
        }
        if (publication_error.type != 0)
        {
            return failure(publication_error);
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
            },
            .configuration_migrations = detail::renderConfigurationMigrations()
        };
    }
} // namespace lux::scene
