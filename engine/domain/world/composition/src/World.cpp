#include <lux/engine/world/World.hpp>
#include <lux/engine/world/WorldSpatialIndex.hpp>

#include <utility>

namespace lux::world
{
    World::World(WorldDescription description) noexcept
        : description_(std::move(description)),
          spatial_indices_(description_.supported_geometries.size())
    {
        identity_connections_[0] = registry_.on_construct<WorldObjectId>().connect<&World::onIdentityCreated>(*this);
        identity_connections_[1] = registry_.on_update<WorldObjectId>().connect<&World::onIdentityUpdated>(*this);
        identity_connections_[2] = registry_.on_destroy<WorldObjectId>().connect<&World::onIdentityDestroyed>(*this);
    }

    World::~World() noexcept = default;

    WorldSpatialIndex& World::spatialIndex(std::uint32_t geometry) noexcept
    {
        if (geometry >= spatial_indices_.size()) std::terminate();
        auto& index = spatial_indices_[geometry];
        if (!index) index = std::make_unique<WorldSpatialIndex>();
        return *index;
    }

    const WorldSpatialIndex& World::spatialIndex(std::uint32_t geometry) const noexcept
    {
        if (geometry >= spatial_indices_.size() || !spatial_indices_[geometry]) std::terminate();
        return *spatial_indices_[geometry];
    }

    ecs::Registry& World::registry() noexcept
    {
        return registry_;
    }

    const ecs::Registry& World::registry() const noexcept
    {
        return registry_;
    }

    const WorldDescription& World::description() const noexcept
    {
        return description_;
    }

    ecs::WorldEntityMap& World::identities() noexcept
    {
        return identities_;
    }

    const ecs::WorldEntityMap& World::identities() const noexcept
    {
        return identities_;
    }

    void World::onIdentityCreated(ecs::Registry& registry, ecs::Entity entity) noexcept
    {
        if (!identities_.bind(registry.get<WorldObjectId>(entity), entity))
        {
            std::terminate(); // Author/load adoption must reject nil or duplicate UUIDs before Registry mutation.
        }
    }

    void World::onIdentityUpdated(ecs::Registry& registry, ecs::Entity entity) noexcept
    {
        if (identities_.entity(registry.get<WorldObjectId>(entity)) != entity)
        {
            std::terminate(); // A persistent identity cannot be reassigned in place.
        }
    }

    void World::onIdentityDestroyed(ecs::Registry& registry, ecs::Entity entity) noexcept
    {
        identities_.unbind(registry.get<WorldObjectId>(entity));
    }
} // namespace lux::world
