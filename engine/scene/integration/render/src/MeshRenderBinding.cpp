#include <bit>
#include <entt/container/dense_set.hpp>
#include <lux/engine/context/render/RenderDiagnostics.hpp>
#include <lux/engine/render/capabilities/RasterAssets.hpp>
#include <lux/engine/scene/render/MeshRenderBinding.hpp>
#include <lux/engine/scene/render/MeshRenderResourceComponent.hpp>
#include <lux/engine/scene/render/RasterAssetPipelines.hpp>
#include <map>

namespace lux::scene
{
    using namespace lux::render;
    using namespace engine::render;
    using namespace simulation::ecs;
    using namespace detail;

    struct MeshRenderBinding::Impl final
    {
        struct Candidate
        {
            Entity entity;
            std::uint64_t serial;
            rdesc::MeshVisualDescription visual;
            std::uint64_t revision;
            asset::AssetVfsView snapshot;
            std::vector<RenderAssetVersion> textures;
            bool mesh_acquired = false, source_acquired = false, material_acquired = false;
            bool exposed = false, pending = false, completed = false, rolling_back = false;
            std::uint64_t input_revision = 0;
            MeshInstanceId instance;
            MeshInstanceDescription description, previous;
            RenderError error;
        };

        struct AssetExit
        {
            RenderAssetVersion key;
            std::uint64_t revision;
        };

        struct InstanceExit
        {
            Entity entity;
            MeshInstanceId instance;
        };

        struct ReleaseBatch
        {
            std::vector<InstanceExit> instances;
            std::size_t released = 0;
            RenderError error;
        };

        Registry& registry;
        RenderAssets& assets;
        RenderAssetInput& asset_input;
        process::asset_loading::AssetReadPort source;
        RenderRequester& client;
        execution::ExecutionRuntime& execution;
        CapabilityInput meshes, textures, materials, instances;
        const std::uint32_t role;
        BoundCommand<CreateMeshInstance> create;
        BoundCommand<UpdateMeshInstances> update;
        BoundCommand<ReleaseMeshInstance> remove;
        std::uint64_t& revision;
        std::uint64_t& adopted;
        std::uint64_t source_revision = 0, next_demand = 0, sequence = 0;
        std::map<Entity, std::uint64_t> intents;
        std::map<Entity, std::unique_ptr<Candidate>> candidates;
        std::map<Entity, RenderError> failures;
        std::map<RenderAssetVersion, entt::dense_set<Entity>> waiters;
        entt::dense_set<Entity> ready;
        std::vector<AssetExit> mesh_exits, material_exits;
        std::vector<InstanceExit> instance_exits;
        std::optional<ReleaseBatch> releases;
        bool releasing = false, closing = false, capacity_blocked = false;
        RenderError fatal;
        RenderAssets::AvailabilityConnection availability;

        Impl(
            RenderBindingContext& context,
            BoundCommand<CreateMeshInstance> create_command,
            BoundCommand<UpdateMeshInstances> update_command,
            BoundCommand<ReleaseMeshInstance> remove_command
        )
            : registry(context.registry), assets(context.assets), asset_input(context.asset_input),
              source(context.source), client(context.client), execution(context.execution), role(context.role_index),
              create(create_command), update(update_command), remove(remove_command), revision(context.input_revision),
              adopted(context.adopted_input_revision), availability(assets, this, &assetReady, &capacityReady)
        {
        }

        static void assetReady(void* data, RenderAssetVersion key) noexcept
        {
            auto& self = *static_cast<Impl*>(data);
            const auto found = self.waiters.find(key);
            if (found != self.waiters.end())
            {
                self.ready.insert(found->second.begin(), found->second.end());
            }
        }

        static void capacityReady(void* data) noexcept
        {
            auto& self = *static_cast<Impl*>(data);
            self.capacity_blocked = false;
            for (const auto& [entity, candidate] : self.candidates)
            {
                self.ready.insert(entity);
            }
            for (const auto& [entity, serial] : self.intents)
            {
                self.ready.insert(entity);
            }
        }

        RenderAssetVersion key(asset::AssetId id, std::uint64_t version) const noexcept
        {
            return {id, version, asset_input.source};
        }

        void changed(Entity entity) noexcept
        {
            intents[entity] = ++next_demand;
            ready.insert(entity);
            failures.erase(entity);
            capacity_blocked = false;
        }

        bool current(const Candidate& candidate) const noexcept
        {
            const auto found = intents.find(candidate.entity);
            return !closing && found != intents.end() && found->second == candidate.serial &&
                   registry.valid(candidate.entity) && registry.all_of<Mesh3D, WorldTransform3D>(candidate.entity);
        }

        template <class P>
        bool acquire(Candidate& candidate, RenderAssetVersion key, CapabilityInput input, bool& acquired) noexcept
        {
            if (acquired)
            {
                return true;
            }
            auto result = assets.acquire<P>(key, source, candidate.snapshot, input);
            if (!result)
            {
                if (result.error().type == kRuntimeCapacity)
                {
                    capacity_blocked = true;
                    availability.whenCapacityAvailable();
                }
                else
                {
                    candidate.error = result.error();
                }
                return false;
            }
            acquired = true;
            waiters[key].insert(candidate.entity);
            availability.observe<P>(key, input);
            return true;
        }

        template <class P>
        const typename P::Value* loaded(
            Candidate& candidate,
            RenderAssetVersion key,
            CapabilityInput input = {}
        ) noexcept
        {
            const auto* value = assets.ready<P>(key, input);
            if (!value)
            {
                candidate.error = assets.failure<P>(key, input);
            }
            return value;
        }

        bool prepareAssets(Candidate& candidate) noexcept
        {
            const auto mesh_key = key(candidate.visual.mesh, candidate.revision);
            const auto material_key = key(candidate.visual.material, candidate.revision);
            if (!acquire<MeshAssetPipeline>(candidate, mesh_key, meshes, candidate.mesh_acquired) ||
                !acquire<MaterialAssetPipeline>(candidate, material_key, {}, candidate.source_acquired))
            {
                return false;
            }
            const auto* material = loaded<MaterialAssetPipeline>(candidate, material_key);
            if (!material)
            {
                return false;
            }
            const auto& cooked = (*material)->data();
            for (std::size_t i = candidate.textures.size(); i < cooked.texture_slot_ids.size(); ++i)
            {
                const auto texture_key = key(cooked.texture_slot_ids[i], candidate.revision);
                if (!texture_key.asset.isNull())
                {
                    bool held = false;
                    if (!acquire<TextureAssetPipeline>(candidate, texture_key, textures, held))
                    {
                        return false;
                    }
                }
                candidate.textures.push_back(texture_key);
            }
            if (!loaded<MeshAssetPipeline>(candidate, mesh_key, meshes))
            {
                return false;
            }
            for (const auto texture : candidate.textures)
            {
                if (!texture.asset.isNull() && !loaded<TextureAssetPipeline>(candidate, texture, textures))
                {
                    return false;
                }
            }
            if (!acquire<MaterialGpuPipeline>(candidate, material_key, materials, candidate.material_acquired))
            {
                return false;
            }
            const auto* gpu_material = loaded<MaterialGpuPipeline>(candidate, material_key, materials);
            if (!gpu_material)
            {
                return false;
            }
            candidate.description.mesh = assets.ready<MeshAssetPipeline>(mesh_key, meshes)->mesh;
            candidate.description.material = gpu_material->material;
            return true;
        }

        void forgetWaiters(const Candidate& candidate) noexcept
        {
            const auto forget = [&](RenderAssetVersion key) noexcept
            {
                const auto entry = waiters.find(key);
                if (entry != waiters.end())
                {
                    entry->second.erase(candidate.entity);
                    if (entry->second.empty())
                    {
                        waiters.erase(entry);
                    }
                }
            };
            forget(key(candidate.visual.mesh, candidate.revision));
            forget(key(candidate.visual.material, candidate.revision));
            for (const auto texture : candidate.textures)
            {
                forget(texture);
            }
        }

        void discard(Candidate& candidate) noexcept
        {
            const auto after = candidate.exposed ? ++revision : 0;
            if (candidate.mesh_acquired)
            {
                mesh_exits.push_back({key(candidate.visual.mesh, candidate.revision), after});
            }
            if (candidate.material_acquired)
            {
                material_exits.push_back({key(candidate.visual.material, candidate.revision), after});
            }
            for (const auto texture : candidate.textures)
            {
                if (!texture.asset.isNull())
                {
                    assets.release<TextureAssetPipeline>(texture, textures);
                }
            }
            if (candidate.source_acquired)
            {
                assets.release<MaterialAssetPipeline>(key(candidate.visual.material, candidate.revision));
            }
            forgetWaiters(candidate);
        }

        exec::task<RenderResult<void>> commit(Candidate& candidate) noexcept
        {
            if (candidate.instance.isNull())
            {
                MeshInstanceCreate request{instances, candidate.description};
                auto response = co_await client.request(create, request);
                candidate.error = response.error;
                candidate.instance = response.instance;
            }
            else
            {
                MeshInstanceUpdate value{
                    candidate.instance,
                    candidate.rolling_back ? candidate.previous : candidate.description
                };
                MeshInstanceBatch request{instances, ++sequence, {&value, 1}};
                auto response = co_await client.request(update, request);
                candidate.error = response.error;
            }
            co_return RenderResult<void>{};
        }

        void start(Candidate& candidate) noexcept
        {
            candidate.pending = true;
            candidate.completed = false;
            auto sender = execution::taskResult(stdexec::starts_on(execution.mainScheduler(), commit(candidate)));
            const bool accepted = bool(execution.spawn(stdexec::then(
                std::move(sender),
                [this, &candidate](auto result) noexcept
                {
                    if (!result)
                    {
                        const auto* domain = result.error().domainFailure();
                        candidate.error = domain ? *domain : RenderError{kRuntimeState};
                    }
                    candidate.pending = false;
                    candidate.completed = true;
                    ready.insert(candidate.entity);
                }
            )));
            if (!accepted)
            {
                std::terminate();
            }
        }

        const MeshRenderResource* resource(Entity entity) const noexcept
        {
            const auto* component =
                registry.valid(entity) ? registry.try_get<MeshRenderResourceComponent>(entity) : nullptr;
            return component ? component->find(role) : nullptr;
        }

        void releaseResource(Entity entity, const MeshRenderResource& value, bool release_instance) noexcept
        {
            const auto after = ++revision;
            mesh_exits.push_back({value.mesh_source, after});
            material_exits.push_back({value.material_source, after});
            if (release_instance)
            {
                instance_exits.push_back({entity, value.instance});
            }
        }

        void removeResource(Entity entity) noexcept
        {
            auto* component = registry.try_get<MeshRenderResourceComponent>(entity);
            if (!component)
            {
                return;
            }
            auto found = std::ranges::find(component->roles, role, &MeshRenderResource::role);
            if (found == component->roles.end())
            {
                return;
            }
            releaseResource(entity, *found, true);
            component->roles.erase(found);
            if (component->roles.empty())
            {
                registry.remove<MeshRenderResourceComponent>(entity); // Empty on_destroy cannot release twice.
            }
            else
            {
                registry.patch<MeshRenderResourceComponent>(entity);
            }
        }

        void adopt(Candidate& candidate) noexcept
        {
            const auto entity = candidate.entity;
            auto& component = registry.get_or_emplace<MeshRenderResourceComponent>(entity);
            const MeshRenderResource value{
                role,
                key(candidate.visual.mesh, candidate.revision),
                key(candidate.visual.material, candidate.revision),
                candidate.description.mesh,
                candidate.description.material,
                candidate.instance,
                candidate.visual
            };
            auto found = std::ranges::find(component.roles, role, &MeshRenderResource::role);
            if (found == component.roles.end())
            {
                component.roles.push_back(value);
            }
            else
            {
                releaseResource(entity, *found, false); // The accepted update kept this role's instance identity.
                *found = value;
            }
            registry.patch<MeshRenderResourceComponent>(entity);
            candidate.mesh_acquired = candidate.material_acquired = false;
            failures.erase(entity);
            intents.erase(entity);
        }

        // Returns true once this finite candidate can be discarded. It never borrows Registry across a wait.
        bool advance(Candidate& candidate) noexcept
        {
            if (candidate.pending)
            {
                return false;
            }
            if (closing && client.serverStopped())
            {
                // Joined Server has already cleared the logical instances. No rollback can be sent;
                // the completed operation's acquired asset references are still returned by discard().
                return true;
            }
            if (candidate.completed)
            {
                if (candidate.rolling_back)
                {
                    if (candidate.error.type != 0)
                    {
                        fatal = candidate.error;
                        return false;
                    }
                    if (registry.valid(candidate.entity) && resource(candidate.entity) != nullptr)
                    {
                        registry.patch<MeshRenderResourceComponent>(candidate.entity);
                    }
                    return true;
                }
                if (candidate.error.type != 0)
                {
                    failures[candidate.entity] = candidate.error;
                    reportRenderFailure(candidate.error, "Adopt raster instance candidate");
                    if (current(candidate))
                    {
                        intents.erase(candidate.entity);
                    }
                    return true;
                }
                if (current(candidate))
                {
                    adopt(candidate);
                    return true;
                }
                if (!candidate.previous.mesh.isNull())
                {
                    // The update was accepted after its author demand changed. Restore the retained old
                    // representation before releasing candidate inputs; do not report a fictitious rollback.
                    candidate.rolling_back = true;
                    start(candidate);
                    return false;
                }
                instance_exits.push_back({candidate.entity, candidate.instance});
                return true;
            }
            if (!current(candidate))
            {
                return true;
            }
            if (!candidate.exposed)
            {
                if (!prepareAssets(candidate))
                {
                    if (candidate.error.type != 0)
                    {
                        failures[candidate.entity] = candidate.error;
                        reportRenderFailure(candidate.error, "Prepare raster asset candidate");
                        intents.erase(candidate.entity);
                        return true;
                    }
                    return false;
                }
                candidate.exposed = true;
                candidate.input_revision = ++revision;
            }
            if (adopted < candidate.input_revision)
            {
                ready.insert(candidate.entity);
                return false;
            }
            const auto& world = registry.get<WorldTransform3D>(candidate.entity);
            Eigen::Map<Eigen::Matrix4f>{candidate.description.transform.data()} = world.value.matrix().cast<float>();
            if (const auto* previous = resource(candidate.entity))
            {
                candidate.instance = previous->instance;
                candidate.previous = candidate.description;
                candidate.previous.mesh = previous->mesh;
                candidate.previous.material = previous->material;
            }
            start(candidate);
            return false;
        }

        exec::task<RenderResult<void>> releaseInstances() noexcept
        {
            auto& work = *releases;
            while (work.released < work.instances.size())
            {
                if (client.serverStopped())
                {
                    work.released = work.instances.size();
                    break;
                }
                auto id = work.instances[work.released].instance;
                const auto response = co_await client.request(remove, id);
                const bool retry =
                    response.error.type == kCommandCancelled || response.error.type == kFrameResynchronization;
                if (retry)
                {
                    co_await client.whenSubmittedFramesComplete();
                    co_await stdexec::schedule(execution.mainScheduler());
                    continue;
                }
                if (response.error.type != 0)
                {
                    work.error = response.error;
                    break;
                }
                ++work.released;
            }
            co_return RenderResult<void>{};
        }

        void drainExits() noexcept
        {
            if (releases && !releasing)
            {
                if (releases->error.type != 0)
                {
                    fatal = releases->error;
                    return;
                }
                releases.reset();
            }
            if (!releases)
            {
                ReleaseBatch batch;
                std::erase_if(
                    instance_exits,
                    [&](const InstanceExit& exit) noexcept
                    {
                        if (candidates.contains(exit.entity))
                        {
                            return false;
                        }
                        batch.instances.push_back(exit);
                        return true;
                    }
                );
                if (!batch.instances.empty())
                {
                    releases.emplace(std::move(batch));
                    releasing = true;
                    auto work =
                        execution::taskResult(stdexec::starts_on(execution.mainScheduler(), releaseInstances()));
                    if (!execution.spawn(stdexec::then(
                            std::move(work),
                            [this](auto result) noexcept
                            {
                                if (!result)
                                {
                                    const auto* domain = result.error().domainFailure();
                                    releases->error = domain ? *domain : RenderError{kRuntimeState};
                                }
                                releasing = false;
                            }
                        )))
                    {
                        std::terminate();
                    }
                }
            }
            if (releases || !instance_exits.empty())
            {
                return;
            }
            const auto ready = [&](const AssetExit& exit) noexcept { return closing || exit.revision <= adopted; };
            std::erase_if(
                material_exits,
                [&](const AssetExit& exit) noexcept
                {
                    if (!ready(exit))
                    {
                        return false;
                    }
                    assets.release<MaterialGpuPipeline>(exit.key, materials);
                    return true;
                }
            );
            std::erase_if(
                mesh_exits,
                [&](const AssetExit& exit) noexcept
                {
                    if (!ready(exit))
                    {
                        return false;
                    }
                    assets.release<MeshAssetPipeline>(exit.key, meshes);
                    return true;
                }
            );
        }
    };

    std::span<const RenderPayloadContract> MeshRenderBinding::inputs() noexcept
    {
        static const auto values = []
        {
            RenderPayloadContract instance;
            instance.schema = kRasterInstancesInput;
            instance.use = ERenderPayloadUse::RESOURCE_INPUT;
            instance.lifetime = ERenderPayloadLifetime::UNTIL_RESPONSE;
            instance.setCppType(cxx::typeToken<MeshInstancesCreate>());
            auto material = instance;
            material.schema = kRasterMaterialInput;
            material.setCppType(cxx::typeToken<MaterialWrite>());
            material.lifetime = ERenderPayloadLifetime::MOVED_VALUE;
            auto mesh = instance;
            mesh.schema = kIndexedMeshInput;
            mesh.setCppType(cxx::typeToken<MeshWrite>());
            auto texture = instance;
            texture.schema = kSampledTextureInput;
            texture.setCppType(cxx::typeToken<TextureWrite>());
            return std::array{instance, material, mesh, texture};
        }();
        return values;
    }

    exec::task<RenderResult<void>> MeshRenderBinding::prepareResources(RenderAssets& assets) noexcept
    {
        co_return co_await installRasterAssetPipelines(assets, assets.resourceCapability());
    }

    MeshRenderBinding::MeshRenderBinding(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    RenderResult<std::unique_ptr<MeshRenderBinding>> MeshRenderBinding::create(RenderBindingContext& context) noexcept
    {
        auto create = context.client.bind<CreateMeshInstance>();
        auto update = context.client.bind<UpdateMeshInstances>();
        auto remove = context.client.bind<ReleaseMeshInstance>();
        auto material_create = context.client.bind<CreateMaterial>();
        auto material_remove = context.client.bind<ReleaseMaterial>();
        if (!create || !update || !remove || !material_create || !material_remove)
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        auto result = std::unique_ptr<MeshRenderBinding>{
            new MeshRenderBinding(std::make_unique<Impl>(context, *create, *update, *remove))
        };
        auto& self = *result->impl_;
        for (const auto& input : context.inputs)
        {
            if (input.schema == kRasterInstancesInput)
            {
                self.instances = input.destination;
            }
            if (input.schema == kRasterMaterialInput)
            {
                self.materials = input.destination;
            }
        }
        for (const auto& input : self.assets.inputs())
        {
            if (input.schema == kIndexedMeshInput)
            {
                self.meshes = input.destination;
            }
            if (input.schema == kSampledTextureInput)
            {
                self.textures = input.destination;
            }
        }
        const bool valid_inputs = !self.instances.implementation.isNull() && !self.materials.implementation.isNull() &&
                                  !self.meshes.implementation.isNull() && !self.textures.implementation.isNull();
        if (!valid_inputs)
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        self.assets.install(
            MaterialGpuPipeline{self.assets, self.materials, self.textures, *material_create, *material_remove},
            self.materials
        );
        return result;
    }

    MeshRenderBinding::~MeshRenderBinding() noexcept
    {
        const auto& self = *impl_;
        if (!self.candidates.empty() || self.releasing || !self.instance_exits.empty() || !self.mesh_exits.empty() ||
            !self.material_exits.empty())
        {
            std::terminate();
        }
    }

    void MeshRenderBinding::sourceChanged(Entity entity) noexcept
    {
        impl_->changed(entity);
    }

    void MeshRenderBinding::resourceDestroyed(Entity entity) noexcept
    {
        auto& self = *impl_;
        if (const auto* value = self.registry.get<MeshRenderResourceComponent>(entity).find(self.role))
        {
            self.releaseResource(entity, *value, true);
        }
    }

    std::uint32_t MeshRenderBinding::role() const noexcept
    {
        return impl_->role;
    }

    SceneStageResult MeshRenderBinding::maintain(SceneStageContext& context) noexcept
    {
        auto& self = *impl_;
        self.closing |= context.stop.stop_requested();
        if (self.closing && self.client.serverStopped())
        {
            self.fatal = {};
            if (self.releases && !self.releasing)
            {
                self.releases->error = {};
            }
        }
        context.publication_needed |= !self.ready.empty() || !self.candidates.empty() || self.releases.has_value() ||
                                      !self.instance_exits.empty() || !self.mesh_exits.empty() ||
                                      !self.material_exits.empty();
        if (!context.allow_structure && !self.closing)
        {
            return ESceneProgress::COMPLETE;
        }
        if (self.fatal.type != 0)
        {
            return cxx::unexpected(SceneExecutionFailure{ESceneExecutionError::SYSTEM_FAILURE, {}, self.fatal});
        }
        if (self.closing)
        {
            for (const auto entity : self.registry.view<MeshRenderResourceComponent>())
            {
                self.removeResource(entity);
            }
            self.intents.clear();
            for (const auto& [entity, candidate] : self.candidates)
            {
                self.ready.insert(entity);
            }
        }
        else if (self.source_revision != self.asset_input.revision)
        {
            self.source_revision = self.asset_input.revision;
            for (const auto entity : self.registry.view<Mesh3D>())
            {
                self.changed(entity);
            }
        }
        auto ready = std::exchange(self.ready, {});
        for (const auto entity : ready)
        {
            auto candidate = self.candidates.find(entity);
            if (candidate != self.candidates.end())
            {
                if (!self.advance(*candidate->second))
                {
                    continue;
                }
                self.discard(*candidate->second);
                self.candidates.erase(candidate);
                if (self.intents.contains(entity))
                {
                    self.ready.insert(entity);
                }
                continue;
            }
            const bool present = !self.closing && self.registry.valid(entity) &&
                                 self.registry.all_of<Mesh3D, WorldTransform3D>(entity) &&
                                 self.registry.get<Mesh3D>(entity).value.visible;
            if (!present)
            {
                if (self.registry.valid(entity))
                {
                    self.removeResource(entity);
                }
                self.intents.erase(entity);
                continue;
            }
            const auto intent = self.intents.find(entity);
            if (intent == self.intents.end())
            {
                continue;
            }
            const auto visual = self.registry.get<Mesh3D>(entity).value;
            const auto* current = self.resource(entity);
            if (current && current->mesh_source.revision == self.source_revision && current->visual == visual)
            {
                self.intents.erase(intent);
                continue;
            }
            auto value = std::make_unique<Impl::Candidate>();
            value->entity = entity;
            value->serial = intent->second;
            value->visual = visual;
            value->revision = self.source_revision;
            value->snapshot = self.asset_input.snapshot;
            auto& inserted = *self.candidates.emplace(entity, std::move(value)).first->second;
            if (self.advance(inserted))
            {
                self.discard(inserted);
                self.candidates.erase(entity);
            }
        }
        self.drainExits();
        if (self.closing)
        {
            // This Scene releases its references, not the shared Global material table.
            // RenderAssets owns remaining global releases and outlives every Scene binding.
            const bool pending = !self.candidates.empty() || self.releases.has_value() ||
                                 !self.instance_exits.empty() || !self.mesh_exits.empty() ||
                                 !self.material_exits.empty();
            return pending ? ESceneProgress::PENDING : ESceneProgress::COMPLETE;
        }
        return ESceneProgress::COMPLETE;
    }

    RenderError MeshRenderBinding::assetError(Entity entity) const noexcept
    {
        const auto found = impl_->failures.find(entity);
        return found == impl_->failures.end() ? RenderError{} : found->second;
    }

    bool MeshRenderBinding::visualReady(Entity entity) const noexcept
    {
        return impl_->registry.valid(entity) && impl_->resource(entity) != nullptr &&
               !impl_->intents.contains(entity) && !impl_->candidates.contains(entity) &&
               !impl_->failures.contains(entity);
    }

    std::uint32_t MeshRenderBinding::instanceCapacity() const noexcept
    {
        std::uint32_t live = 0;
        for (const auto entity : impl_->registry.view<MeshRenderResourceComponent>())
        {
            live += impl_->resource(entity) != nullptr;
        }
        return std::bit_ceil(std::max<std::uint32_t>(128, static_cast<std::uint32_t>(live + impl_->candidates.size())));
    }

    std::vector<RasterGeometryInput> MeshRenderBinding::geometry() const noexcept
    {
        const auto& self = *impl_;
        std::vector<RasterGeometryInput> result;
        std::unordered_multimap<std::uint64_t, std::size_t> indices;
        std::vector<const rdesc::MaterialProgramSet*> programs;
        const auto append = [&](RenderAssetVersion mesh_key, RenderAssetVersion material_key)
        {
            const auto& mesh = *self.assets.ready<MeshAssetPipeline>(mesh_key, self.meshes);
            const auto& material = *self.assets.ready<MaterialGpuPipeline>(material_key, self.materials);
            const auto& program = cookedMaterial(self.assets, material_key)->programs;
            const auto bucket = material.program_hash ^ mesh.mesh.hash();
            const auto [first, last] = indices.equal_range(bucket);
            auto found = first;
            for (; found != last; ++found)
            {
                const auto i = found->second;
                if (result[i].selection.mesh == mesh.mesh && (programs[i] == &program || *programs[i] == program))
                {
                    break;
                }
            }
            const RasterGeometrySelection selection{mesh.mesh, material.material};
            std::size_t i;
            if (found == last)
            {
                i = result.size();
                indices.emplace(bucket, i);
                programs.push_back(&program);
                result.push_back({mesh.geometry, program, selection});
            }
            else
            {
                i = found->second;
                if (!(selection < result[i].selection))
                {
                    return;
                }
                result[i].selection = selection;
                result[i].textures.clear();
            }
            for (const auto texture : material.textures)
            {
                const auto* value =
                    texture.asset.isNull() ? nullptr : self.assets.ready<TextureAssetPipeline>(texture, self.textures);
                result[i].textures.push_back(
                    value ? value->description : ImageShape{1, 1, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM}
                );
            }
        };
        for (const auto entity : self.registry.view<const MeshRenderResourceComponent>())
        {
            if (const auto* value = self.resource(entity))
            {
                append(value->mesh_source, value->material_source);
            }
        }
        for (const auto& [entity, candidate] : self.candidates)
        {
            if (candidate->exposed)
            {
                append(
                    self.key(candidate->visual.mesh, candidate->revision),
                    self.key(candidate->visual.material, candidate->revision)
                );
            }
        }
        std::ranges::sort(
            result,
            [](const auto& left, const auto& right) noexcept { return left.selection < right.selection; }
        );
        return result;
    }
} // namespace lux::scene
