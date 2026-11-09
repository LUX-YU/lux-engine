#pragma once
#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/world/WorldObjectId.hpp>
#include <unordered_map>

namespace lux::ecs
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
        }
        [[nodiscard]] std::size_t size() const noexcept
        {
            return objects_.size();
        }
        // Owner-thread view, invalidated by bind/unbind. Callers cannot mutate either index directly.
        [[nodiscard]] const auto& entries() const noexcept
        {
            return objects_;
        }
        [[nodiscard]] Entity entity(world::WorldObjectId object) const noexcept
        {
            const auto found = objects_.find(object);
            return found == objects_.end() ? NullEntity : found->second;
        }
        // The Registry owner maintains the Entity's identity component. Rebinding the same pair is harmless.
        // A conflicting UUID never changes the existing mapping. This index stores no reverse lookup.
        [[nodiscard]] bool bind(world::WorldObjectId object, Entity entity)
        {
            const bool is_invalid_identity = !world::isValid(object) || entity == NullEntity;
            if (is_invalid_identity)
            {
                return false;
            }
            const auto [position, inserted] = objects_.emplace(object, entity);
            return inserted || position->second == entity;
        }
        void unbind(world::WorldObjectId object) noexcept
        {
            objects_.erase(object);
        }

    private:
        std::unordered_map<world::WorldObjectId, Entity, world::WorldObjectIdHash> objects_;
    };
}
