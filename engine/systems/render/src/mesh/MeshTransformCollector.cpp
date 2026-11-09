#include <lux/engine/systems/render/mesh/MeshTransformCollector.hpp>
#include <lux/engine/systems/render/mesh/MeshRenderResourceComponent.hpp>
#include <lux/engine/world/World.hpp>
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/Visual.hpp>
#include <lux/engine/ecs/VisibilityComponent.hpp>
#include <lux/engine/ecs/RenderableMeshComponent.hpp>

namespace lux::engine::render
{
    namespace
    {
        lux::render::TransformMatrix transformMatrix(const ecs::Transform3D* transform) noexcept
        {
            if (!transform) return lux::render::kIdentityTransform;
            const Eigen::Matrix4f matrix = (Eigen::Translation3d(transform->translation) * transform->rotation *
                Eigen::Scaling(transform->scale)).matrix().cast<float>();
            lux::render::TransformMatrix result;
            std::copy_n(matrix.data(), result.size(), result.data());
            return result;
        }
    }

    MeshTransformCollector::MeshTransformCollector(
        RenderSystem& system,
        lux::render::FrameDataDestination destination
    ) noexcept
        : registry_(system.world().registry()),
          assets_(system.context().assets()),
          output_(lux::render::frameDataBinding<lux::render::Mesh3dCapability, lux::render::TransformData>(
              destination.effect, destination.capability, destination.scope
          )),
          properties_output_(lux::render::frameDataBinding<
              lux::render::Mesh3dCapability, lux::render::MeshInstanceUpdates
          >(destination.effect, destination.capability, destination.scope))
    {
        connections_[0] = registry_.on_construct<ecs::Transform3D>()
            .connect<&MeshTransformCollector::transformChanged>(*this);
        connections_[1] = registry_.on_update<ecs::Transform3D>()
            .connect<&MeshTransformCollector::transformChanged>(*this);
        connections_[2] = registry_.on_destroy<ecs::Transform3D>()
            .connect<&MeshTransformCollector::propertiesChanged>(*this);
        connections_[3] = registry_.on_construct<MeshRenderResourceComponent>()
            .connect<&MeshTransformCollector::propertiesChanged>(*this);
        connections_[4] = registry_.on_update<MeshRenderResourceComponent>()
            .connect<&MeshTransformCollector::propertiesChanged>(*this);
        connections_[5] = registry_.on_destroy<MeshRenderResourceComponent>()
            .connect<&MeshTransformCollector::resourcesRemoved>(*this);
        std::size_t connection = 6;
        const auto connect = [&]<class Component>() noexcept
        {
            connections_[connection++] = registry_.on_construct<Component>()
                .template connect<&MeshTransformCollector::propertiesChanged>(*this);
            connections_[connection++] = registry_.on_update<Component>()
                .template connect<&MeshTransformCollector::propertiesChanged>(*this);
            connections_[connection++] = registry_.on_destroy<Component>()
                .template connect<&MeshTransformCollector::propertiesChanged>(*this);
        };
        connect.template operator()<ecs::Mesh3D>();
        connect.template operator()<ecs::MaterialComponent>();
        connect.template operator()<ecs::RenderableMeshComponent>();
        connect.template operator()<ecs::VisibilityComponent>();
        for (auto entity : registry_.view<MeshRenderResourceComponent>()) pending_properties_.insert(entity);
    }

    void MeshTransformCollector::transformChanged(ecs::Registry& registry, ecs::Entity entity) noexcept
    {
        if (registry.all_of<MeshRenderResourceComponent>(entity)) pending_.insert(entity);
    }

    void MeshTransformCollector::propertiesChanged(ecs::Registry& registry, ecs::Entity entity) noexcept
    {
        if (registry.all_of<MeshRenderResourceComponent>(entity)) pending_properties_.insert(entity);
    }

    void MeshTransformCollector::resourcesRemoved(ecs::Registry&, ecs::Entity entity) noexcept
    {
        pending_.erase(entity);
        pending_properties_.erase(entity);
    }

    MeshTransformCollector::CreateResult
    MeshTransformCollector::create(RenderSystem& system, lux::render::FrameDataDestination destination) noexcept
    {
        return std::unique_ptr<MeshTransformCollector>{new MeshTransformCollector{system, destination}};
    }

    void MeshTransformCollector::invalidate() noexcept
    {
        resynchronize_ = true;
    }

    lux::render::RenderResult<void> MeshTransformCollector::collect(lux::render::FrameWriter& frame) noexcept
    {
        if (resynchronize_)
        {
            for (auto entity : registry_.view<MeshRenderResourceComponent>()) pending_properties_.insert(entity);
        }
        std::size_t property_count = 0, transform_count = 0;
        for (auto entity : pending_properties_)
        {
            const auto* resources = registry_.valid(entity) ?
                registry_.try_get<MeshRenderResourceComponent>(entity) : nullptr;
            if (resources) property_count += resources->instances.size();
        }
        for (auto entity : pending_)
        {
            const bool present = !pending_properties_.contains(entity) && registry_.valid(entity) &&
                registry_.all_of<ecs::Transform3D, MeshRenderResourceComponent>(entity);
            if (present) transform_count += registry_.get<MeshRenderResourceComponent>(entity).instances.size();
        }
        if (property_count)
        {
            auto updates = frame.allocate<lux::render::MeshInstanceUpdate>(property_count);
            if (!updates) return cxx::unexpected(updates.error());
            std::size_t index = 0;
            for (auto entity : pending_properties_)
            {
                const auto* resources = registry_.valid(entity) ?
                    registry_.try_get<MeshRenderResourceComponent>(entity) : nullptr;
                if (!resources) continue;
                const auto* transform = registry_.try_get<ecs::Transform3D>(entity);
                const auto* mesh = registry_.try_get<ecs::Mesh3D>(entity);
                const auto* materials = registry_.try_get<ecs::MaterialComponent>(entity);
                const auto* renderable = registry_.try_get<ecs::RenderableMeshComponent>(entity);
                const auto* visibility = registry_.try_get<ecs::VisibilityComponent>(entity);
                bool enabled = transform && mesh && materials && renderable &&
                    visibility && visibility->is_visible &&
                    visibility->global_requirement == ecs::EVisibilityRequirement::CANDIDATE &&
                    resources->mesh && resources->mesh.assetId() == mesh->value.mesh &&
                    resources->materials.size() == materials->materials.size() && assets_.isCurrent(resources->mesh);
                if (enabled)
                {
                    for (std::size_t i = 0; i < resources->materials.size(); ++i)
                    {
                        if (resources->materials[i].assetId() != materials->materials[i] ||
                            !assets_.isCurrent(resources->materials[i])) { enabled = false; break; }
                    }
                }
                const auto matrix = transformMatrix(transform);
                for (std::size_t i = 0; i < resources->instances.size(); ++i)
                {
                    updates->values[index++] = {resources->instances[i], {
                        *resources->mesh, *resources->materials[i], static_cast<std::uint32_t>(i), matrix,
                        enabled, renderable && renderable->cast_shadow
                    }};
                }
            }
            auto written = frame.write(properties_output_, {0, updates->range});
            if (!written) return written;
        }
        if (transform_count)
        {
            auto instances = frame.allocate<lux::render::MeshInstanceId>(transform_count);
            if (!instances) return cxx::unexpected(instances.error());
            auto transforms = frame.allocate<lux::render::TransformMatrix>(transform_count);
            if (!transforms) return cxx::unexpected(transforms.error());
            std::size_t index = 0;
            for (auto entity : pending_)
            {
                const bool present = !pending_properties_.contains(entity) && registry_.valid(entity) &&
                    registry_.all_of<ecs::Transform3D, MeshRenderResourceComponent>(entity);
                if (!present) continue;
                const auto matrix = transformMatrix(&registry_.get<ecs::Transform3D>(entity));
                for (auto instance : registry_.get<MeshRenderResourceComponent>(entity).instances)
                {
                    instances->values[index] = instance;
                    transforms->values[index++] = matrix;
                }
            }
            auto written = frame.write(output_, {0, instances->range, transforms->range});
            if (!written) return written;
        }
        // Only frame construction succeeded. RenderSystem invalidates all adopted data on failed/partial delivery.
        pending_.clear();
        pending_properties_.clear();
        resynchronize_ = false;
        return {};
    }
}
