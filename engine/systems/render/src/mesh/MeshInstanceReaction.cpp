#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/Visual.hpp>
#include <lux/engine/ecs/VisibilityComponent.hpp>
#include <lux/engine/ecs/RenderableMeshComponent.hpp>
#include <lux/engine/systems/render/RenderSystem.hpp>
#include <lux/engine/systems/render/mesh/MeshInstanceReaction.hpp>
#include <lux/engine/systems/render/mesh/MeshRenderResourceComponent.ecs_schema.hpp>
#include <lux/engine/systems/render/mesh/MeshRenderResourceComponent.hpp>
#include <lux/engine/world/World.hpp>
#include <utility>

namespace lux::engine::render
{
    namespace
    {
        lux::render::TransformMatrix meshTransform(const ecs::Transform3D& transform) noexcept
        {
            const Eigen::Matrix4f matrix =
                (Eigen::Translation3d(transform.translation) * transform.rotation * Eigen::Scaling(transform.scale))
                    .matrix()
                    .cast<float>();
            lux::render::TransformMatrix result;
            std::copy_n(matrix.data(), result.size(), result.data());
            return result;
        }
    } // namespace

    MeshInstanceReaction::MeshInstanceReaction(
        RenderSystem& system,
        lux::render::CapabilityImplementationId implementation,
        MeshUpload& meshes,
        MaterialUpload& materials,
        lux::render::CommandId create_instances,
        lux::render::CommandId release_instance
    ) noexcept
        : RenderReaction(system), meshes_(meshes), materials_(materials), implementation_(implementation),
          create_instances_id_(create_instances), release_instance_id_(release_instance)
    {
        watchComponents<ecs::Mesh3D, ecs::MaterialComponent, ecs::RenderableMeshComponent, ecs::VisibilityComponent>();
        connectConstruct<ecs::Transform3D, &MeshInstanceReaction::inputChanged>(*this);
        connectRemoval<ecs::Transform3D, &MeshInstanceReaction::inputChanged>(*this);
        connectRemoval<MeshRenderResourceComponent, &MeshInstanceReaction::removeResources>(*this);
    }

    MeshInstanceReaction::~MeshInstanceReaction() noexcept = default;

    MeshInstanceReaction::CreateResult MeshInstanceReaction::create(
        RenderSystem& system,
        lux::render::CapabilityImplementationId implementation,
        MeshUpload& meshes,
        MaterialUpload& materials,
        lux::render::CommandId create_instances,
        lux::render::CommandId release_instance
    ) noexcept
    {
        const bool invalid_commands = create_instances.isNull() || release_instance.isNull();
        if (invalid_commands)
        {
            return cxx::unexpected(lux::render::RenderError{lux::render::kCommandContract});
        }
        return std::unique_ptr<MeshInstanceReaction>{
            new MeshInstanceReaction{system, implementation, meshes, materials, create_instances, release_instance}
        };
    }

    void MeshInstanceReaction::inputChanged(ecs::Registry& registry, ecs::Entity entity) noexcept
    {
        if (registry.any_of<ecs::Mesh3D, MeshRenderResourceComponent>(entity)) notify(entity);
    }

    void MeshInstanceReaction::reportFailure(ecs::Entity entity, error::Error error) noexcept
    {
        if (error.type == lux::render::kDeviceBusy)
        {
            notify(entity); // Retry at the next Engine round, never in a tight completion loop.
            return;
        }
        static_cast<void>(emit(preparationFailed, PreparationFailure{entity, error}));
    }

    void MeshInstanceReaction::removeResources(ecs::Registry& registry, ecs::Entity entity) noexcept
    {
        auto resources = std::exchange(registry.get<MeshRenderResourceComponent>(entity), {});
        const bool empty = !resources.mesh && resources.materials.empty() && resources.instances.empty();
        if (!empty)
        {
            removed_resources_.emplace(entity, std::move(resources));
            notify(entity);
        }
    }

    MeshRenderResourceComponent MeshInstanceReaction::takeResources(ecs::Entity entity) noexcept
    {
        auto& registry = this->registry();
        auto* adopted = registry.valid(entity) ? registry.try_get<MeshRenderResourceComponent>(entity) : nullptr;
        if (!adopted)
        {
            return {};
        }
        auto resources = std::exchange(*adopted, {});
        registry.remove<MeshRenderResourceComponent>(entity);
        return resources;
    }

    void MeshInstanceReaction::forgetAssets(ecs::Entity entity) noexcept
    {
        const auto found = asset_inputs_.find(entity);
        if (found == asset_inputs_.end()) return;
        for (auto id : found->second)
        {
            const auto observer = asset_observers_.find(id);
            if (observer == asset_observers_.end()) continue;
            auto& [reference, subscription] = observer->second;
            auto& [connection, users] = subscription;
            users.erase(entity);
            if (users.empty()) asset_observers_.erase(observer);
        }
        asset_inputs_.erase(found);
    }

    MeshInstanceReaction::Result MeshInstanceReaction::refreshAssets(ecs::Entity entity) noexcept
    {
        const auto& material_ids = registry().get<ecs::MaterialComponent>(entity).materials;
        const auto mesh_id = registry().get<ecs::Mesh3D>(entity).value.mesh;
        const auto previous = asset_inputs_.find(entity);
        const bool same_inputs = previous != asset_inputs_.end() &&
            previous->second.size() == material_ids.size() + 1 && previous->second.front() == mesh_id &&
            std::equal(material_ids.begin(), material_ids.end(), previous->second.begin() + 1);
        if (same_inputs) return {};
        std::vector<asset::AssetId> inputs;
        inputs.reserve(material_ids.size() + 1);
        inputs.push_back(mesh_id);
        inputs.insert(inputs.end(), material_ids.begin(), material_ids.end());
        auto& repository = system().context().assets();
        std::vector<assets::AssetRef<asset::Asset>> references;
        references.reserve(inputs.size());
        auto mesh = repository.acquire<asset::MeshAsset>(inputs.front());
        if (!mesh) return cxx::unexpected(mesh.error());
        references.emplace_back(*mesh);
        for (auto id : material_ids)
        {
            auto material = repository.acquire<asset::MaterialAsset>(id);
            if (!material) return cxx::unexpected(material.error());
            references.emplace_back(*material);
        }
        forgetAssets(entity);
        for (auto& reference : references)
        {
            auto [observer, inserted] = asset_observers_.try_emplace(reference.id());
            auto& [retained, subscription] = observer->second;
            auto& [connection, users] = subscription;
            if (inserted)
            {
                retained = reference;
                connection = repository.observe<&MeshInstanceReaction::onAssetChanged>(reference, *this);
            }
            users.insert(entity);
        }
        asset_inputs_.emplace(entity, std::move(inputs));
        return {};
    }

    void MeshInstanceReaction::onAssetChanged(asset::AssetId asset) noexcept
    {
        const auto found = asset_observers_.find(asset);
        if (found == asset_observers_.end()) return;
        const auto& [reference, subscription] = found->second;
        const auto& [connection, users] = subscription;
        for (auto entity : users) notify(entity);
    }

    exec::task<MeshInstanceReaction::Result> MeshInstanceReaction::reconcile(ecs::Entity entity) noexcept
    {
        auto drained = co_await drainRemoved(entity);
        if (!drained) co_return drained;
        auto& registry = this->registry();
        auto& repository = system().context().assets();
        const bool present = canAdopt(entity) && registry.all_of<
            ecs::Mesh3D, ecs::MaterialComponent, ecs::Transform3D, ecs::RenderableMeshComponent
        >(entity);
        if (!present)
        {
            forgetAssets(entity);
            co_return co_await discard(entity, takeResources(entity));
        }
        if (auto observed = refreshAssets(entity); !observed) co_return observed;
        // Asset notifications only mark intent. At this Main safe point notify the collector of stale uses.
        if (registry.all_of<MeshRenderResourceComponent>(entity))
            registry.patch<MeshRenderResourceComponent>(entity);
        const auto requirement = [&]() noexcept
        {
            const auto* visibility = registry.try_get<ecs::VisibilityComponent>(entity);
            return visibility && visibility->is_visible ? visibility->global_requirement :
                ecs::EVisibilityRequirement::NONE;
        };
        const auto keep = [&]() noexcept
        {
            return canAdopt(entity) && registry.all_of<ecs::RenderableMeshComponent>(entity) &&
                (requirement() != ecs::EVisibilityRequirement::NONE ||
                 registry.get<ecs::RenderableMeshComponent>(entity).keep_resources_when_hidden);
        };
        const auto mesh_id = registry.get<ecs::Mesh3D>(entity).value.mesh;
        const auto material_ids = registry.get<ecs::MaterialComponent>(entity).materials;
        const auto matches = [&](const MeshRenderResourceComponent& resources) noexcept
        {
            const bool has_inputs = canAdopt(entity) && registry.all_of<
                ecs::Mesh3D, ecs::MaterialComponent, ecs::Transform3D, ecs::RenderableMeshComponent
            >(entity);
            if (!has_inputs || registry.get<ecs::Mesh3D>(entity).value.mesh != mesh_id ||
                registry.get<ecs::MaterialComponent>(entity).materials != material_ids) return false;
            if (!resources.mesh || resources.mesh.assetId() != mesh_id || !repository.isCurrent(resources.mesh) ||
                resources.materials.size() != material_ids.size()) return false;
            for (std::size_t i = 0; i < material_ids.size(); ++i)
            {
                const bool is_stale_material = resources.materials[i].assetId() != material_ids[i] ||
                    !repository.isCurrent(resources.materials[i]);
                if (is_stale_material) return false;
            }
            return true;
        };
        if (!keep()) co_return co_await discard(entity, takeResources(entity));
        if (const auto* adopted = registry.try_get<MeshRenderResourceComponent>(entity); adopted && matches(*adopted))
            co_return Result{};
        // Retention preserves an existing or accepted representation; it never starts new preparation.
        if (requirement() != ecs::EVisibilityRequirement::CANDIDATE) co_return Result{};
        if (material_ids.empty()) co_return cxx::unexpected(error::Error{lux::render::kCommandContract});
        MeshRenderResourceComponent candidate;
        auto asset = repository.acquire<asset::MeshAsset>(mesh_id);
        if (!asset) co_return cxx::unexpected(asset.error());
        auto mesh = co_await repository.use<MeshUploadDefinition>(*asset, meshes_);
        if (!mesh) co_return cxx::unexpected(mesh.error());
        candidate.mesh = std::move(*mesh);
        for (const auto id : material_ids)
        {
            auto material_asset = repository.acquire<asset::MaterialAsset>(id);
            if (!material_asset) co_return cxx::unexpected(material_asset.error());
            auto material = co_await repository.use<MaterialUploadDefinition>(*material_asset, materials_);
            if (!material) co_return cxx::unexpected(material.error());
            candidate.materials.push_back(std::move(*material));
        }
        if (!matches(candidate) || !keep())
        {
            notify(entity);
            co_return Result{};
        }
        const auto transform = meshTransform(registry.get<ecs::Transform3D>(entity));
        std::vector<lux::render::MeshInstanceData> instances;
        instances.reserve(candidate.materials.size());
        for (std::size_t i = 0; i < candidate.materials.size(); ++i)
        {
            // Disabled until the adopted shadow's Collector publishes the complete current input.
            instances.push_back({*candidate.mesh, *candidate.materials[i], static_cast<std::uint32_t>(i), transform});
        }
        lux::render::Mesh3dCommands::CreateInstances::Request request{implementation_, system().sceneId(), instances};
        auto& client = system().context().renderClient();
        auto response = co_await client.request<lux::render::Mesh3dCommands::CreateInstances>(
            create_instances_id_, request
        );
        candidate.instances = std::move(response.instances);
        if (response.error.type)
        {
            auto cleaned = co_await discard(entity, std::move(candidate));
            co_return cleaned ? Result{cxx::unexpected(response.error)} : std::move(cleaned);
        }
        if (!matches(candidate) || !keep())
        {
            notify(entity);
            co_return co_await discard(entity, std::move(candidate));
        }
        auto previous = takeResources(entity);
        registry.emplace<MeshRenderResourceComponent>(entity, std::exchange(candidate, {}));
        co_return co_await discard(entity, std::move(previous));
    }

    exec::task<MeshInstanceReaction::Result>
    MeshInstanceReaction::releaseMeshResources(MeshRenderResourceComponent& resources) noexcept
    {
        auto& client = system().context().renderClient();
        while (!resources.instances.empty())
        {
            lux::render::Mesh3dCommands::ReleaseInstance::Request request{resources.instances.back()};
            auto response = co_await client.request<lux::render::Mesh3dCommands::ReleaseInstance>(
                release_instance_id_,
                request
            );
            if (response.error.type)
            {
                co_return cxx::unexpected(response.error);
            }
            resources.instances.pop_back();
        }
        resources.materials.clear();
        resources.mesh = {};
        co_return MeshInstanceReaction::Result{};
    }

    exec::task<MeshInstanceReaction::Result>
    MeshInstanceReaction::discard(ecs::Entity entity, MeshRenderResourceComponent resources) noexcept
    {
        auto released = co_await releaseMeshResources(resources);
        if (!released)
        {
            removed_resources_.emplace(entity, std::move(resources));
            if (released.error().type != lux::render::kDeviceBusy)
            {
                RenderReaction::reportFailure(entity, released.error());
            }
        }
        co_return released;
    }

    exec::task<MeshInstanceReaction::Result> MeshInstanceReaction::drainRemoved(ecs::Entity entity) noexcept
    {
        for (;;)
        {
            const auto found = removed_resources_.find(entity);
            if (found == removed_resources_.end())
            {
                co_return MeshInstanceReaction::Result{};
            }
            auto node = removed_resources_.extract(found);
            auto released = co_await releaseMeshResources(node.mapped());
            if (!released)
            {
                removed_resources_.insert(std::move(node));
                if (released.error().type != lux::render::kDeviceBusy)
                {
                    RenderReaction::reportFailure(entity, released.error());
                }
                co_return released;
            }
        }
    }

    exec::task<MeshInstanceReaction::Result> MeshInstanceReaction::releaseResources() noexcept
    {
        asset_observers_.clear();
        asset_inputs_.clear();
        while (!removed_resources_.empty())
        {
            auto released = co_await drainRemoved(removed_resources_.begin()->first);
            if (!released)
            {
                co_return released;
            }
        }
        for (;;)
        {
            auto adopted = registry().view<MeshRenderResourceComponent>();
            if (adopted.begin() == adopted.end())
            {
                co_return MeshInstanceReaction::Result{};
            }
            const auto entity = *adopted.begin();
            auto released = co_await discard(entity, takeResources(entity));
            if (!released)
            {
                co_return released;
            }
        }
    }
} // namespace lux::engine::render
