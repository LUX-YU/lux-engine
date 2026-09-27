#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/SceneSystemInstaller.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <entt/signal/sigh.hpp>

namespace lux::scene
{
    using simulation::ecs::Entity;
    using simulation::ecs::Mesh3D;
    using simulation::ecs::Registry;
    using simulation::ecs::WorldTransform3D;

    struct MeshQuerySystem::Impl final
    {
        explicit Impl(Registry& value)
            : registry_(value), query_(value.ctx().emplace<MeshQuery>()),
              mesh_added_(value.on_construct<Mesh3D>().connect<&Impl::structureChanged>(*this)),
              mesh_changed_(value.on_update<Mesh3D>().connect<&Impl::structureChanged>(*this)),
              mesh_removed_(value.on_destroy<Mesh3D>().connect<&Impl::structureChanged>(*this)),
              pose_added_(value.on_construct<WorldTransform3D>().connect<&Impl::structureChanged>(*this)),
              pose_changed_(value.on_update<WorldTransform3D>().connect<&Impl::transformChanged>(*this)),
              pose_removed_(value.on_destroy<WorldTransform3D>().connect<&Impl::structureChanged>(*this))
        {
            // The initial rebuild folds all existing components; signals only describe later changes.
            query_.structureChanged();
        }
        void structureChanged(Registry&, Entity) noexcept
        {
            query_.structureChanged();
        }
        void transformChanged(Registry& registry, Entity entity)
        {
            query_.transformChanged(registry, entity);
        }
        void update()
        {
            query_.update(registry_);
        }
        bool hasPendingChanges() const noexcept
        {
            return query_.hasPendingChanges();
        }

        Registry& registry_;
        MeshQuery& query_;
        entt::scoped_connection mesh_added_, mesh_changed_, mesh_removed_;
        entt::scoped_connection pose_added_, pose_changed_, pose_removed_;
    };

    MeshQuerySystem::MeshQuerySystem(Registry& registry) : impl_(std::make_unique<Impl>(registry)) {}
    MeshQuerySystem::~MeshQuerySystem() noexcept = default;
    void MeshQuerySystem::updateStablePoint()
    {
        impl_->update();
    }
    bool MeshQuerySystem::hasPendingChanges() const noexcept
    {
        return impl_->hasPendingChanges();
    }

    SceneSystemRegistration builtinMeshQuerySystemRegistration() noexcept
    {
        return {
            .type = system::systemTypeId(MeshQuerySystem::Description.canonical_name),
            .cpp_type = lux::cxx::typeToken<MeshQuerySystem>(),
            .description = &MeshQuerySystem::Description,
            .install = +[](SceneSystemInstaller& builder, SceneSystemDescription description
                        ) noexcept -> lux::cxx::expected<void, SceneSystemBuildFailure> {
                auto instance = builder.emplaceSystem<MeshQuerySystem>(description.instanceId(), builder.registry());
                if (!instance)
                {
                    return lux::cxx::unexpected(instance.error());
                }
                const auto maintenance = builder.addMaintenanceTask<MeshQuerySystem>(
                    description.instanceId(),
                    [](MeshQuerySystem& query, SceneStageContext& context) noexcept -> SceneStageResult {
                        context.publication_needed |= query.hasPendingChanges();
                        return ESceneProgress::COMPLETE;
                    }
                );
                if (!maintenance)
                {
                    return maintenance;
                }
                return builder.addStablePointTask<MeshQuerySystem>(
                    description.instanceId(),
                    [](MeshQuerySystem& query) noexcept -> SceneStageResult {
                        query.updateStablePoint();
                        return ESceneProgress::COMPLETE;
                    }
                );
            }
        };
    }
} // namespace lux::scene
