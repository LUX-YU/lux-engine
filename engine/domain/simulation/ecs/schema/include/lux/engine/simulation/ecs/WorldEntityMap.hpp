#pragma once
#include <lux/engine/simulation/ecs/Entity.hpp>
#include <lux/engine/world/WorldObjectId.hpp>
#include <unordered_map>

namespace lux::simulation::ecs
{
    // One world's resident identities. The Registry owner binds/unbinds at its structural safe point.
    // This index owns no entities and never serializes EnTT index/generation bits.
    class WorldEntityMap final
    {
    public:
        WorldEntityMap() = default;
        WorldEntityMap(const WorldEntityMap&) = delete;
        WorldEntityMap& operator=(const WorldEntityMap&) = delete;
        WorldEntityMap(WorldEntityMap&&) noexcept = default;
        WorldEntityMap& operator=(WorldEntityMap&&) noexcept = default;

        void reserve(std::size_t count)
        {
            objects_.reserve(count);
            entities_.reserve(count);
        }
        [[nodiscard]] std::size_t size() const noexcept { return objects_.size(); }
        // Owner-thread view, invalidated by bind/unbind. Callers cannot mutate either index directly.
        [[nodiscard]] const auto& entries() const noexcept { return objects_; }
        [[nodiscard]] Entity entity(world::WorldObjectId object) const noexcept
        {
            const auto found = objects_.find(object);
            return found == objects_.end() ? NullEntity : found->second;
        }
        [[nodiscard]] world::WorldObjectId object(Entity entity) const noexcept
        {
            const auto found = entities_.find(entity);
            return found == entities_.end() ? world::WorldObjectId{} : found->second;
        }
        // Semantic rejection changes neither direction. Allocation failure is not recovered.
        [[nodiscard]] bool bind(world::WorldObjectId object, Entity entity)
        {
            if (!object.valid() || entity == NullEntity) return false;
            if (objects_.contains(object) || entities_.contains(entity)) return false;
            objects_.emplace(object, entity);
            entities_.emplace(entity, object);
            return true;
        }
        void unbind(Entity entity) noexcept
        {
            const auto found = entities_.find(entity);
            if (found == entities_.end()) return;
            objects_.erase(found->second);
            entities_.erase(found);
        }

    private:
        std::unordered_map<world::WorldObjectId, Entity, world::WorldObjectIdHash> objects_;
        std::unordered_map<Entity, world::WorldObjectId> entities_;
    };
}
