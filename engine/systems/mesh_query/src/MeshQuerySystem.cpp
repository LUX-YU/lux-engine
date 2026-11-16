#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/ecs/Transform.hpp>
#include <lux/engine/ecs/Visual.hpp>
#include <lux/engine/world/World.hpp>
#include <entt/signal/sigh.hpp>

namespace lux::scene
{
    using ecs::Entity;
    using ecs::Mesh3D;
    using ecs::Registry;
    using ecs::WorldTransform3D;

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

    SceneSystem::Result MeshQuerySystem::execute() noexcept
    {
        if (impl_->hasPendingChanges())
        {
            impl_->update();
        }
        return {};
    }

    SceneSystemRegistration builtinMeshQuerySystemRegistration() noexcept
    {
        return {
            .name = std::string{MeshQuerySystem::Name},
            .create = +[](engine::EngineContext&, world::World& world, simulation::Simulation&,
                          const ecs::ComponentSchemaSet&, std::span<const std::byte> bytes
            ) noexcept -> cxx::expected<std::unique_ptr<SceneSystem>, error::Error>
            {
                const bool invalid_configuration = !bytes.empty() || world.registry().ctx().contains<MeshQuery>();
                if (invalid_configuration)
                {
                    return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_ARGUMENT));
                }
                return std::unique_ptr<SceneSystem>{std::make_unique<MeshQuerySystem>(world.registry())};
            }
        };
    }
} // namespace lux::scene
