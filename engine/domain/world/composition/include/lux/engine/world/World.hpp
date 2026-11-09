#pragma once

#include <lux/engine/ecs/Registry.hpp>
#include <lux/engine/ecs/WorldEntityMap.hpp>
#include <lux/engine/world/WorldDescription.hpp>
#include <lux/engine/world/composition/visibility.h>

#include <array>
#include <span>
#include <memory>
#include <vector>

namespace lux::ecs
{
    struct ComponentSchema;
}

namespace lux::world
{
    class WorldSpatialIndex;
    // Cold component registration; generated author data schemas contain no System dependencies.
    [[nodiscard]] LUX_ENGINE_WORLD_COMPOSITION_PUBLIC std::span<const ecs::ComponentSchema>
    worldComponentSchemas() noexcept;

    // Owns the world's composition data and entities; systems only borrow them.
    class LUX_ENGINE_WORLD_COMPOSITION_PUBLIC World final
    {
    public:
        explicit World(WorldDescription description) noexcept;
        ~World() noexcept;
        World(const World&) = delete;
        World& operator=(const World&) = delete;

        [[nodiscard]] ecs::Registry& registry() noexcept;
        [[nodiscard]] const ecs::Registry& registry() const noexcept;
        [[nodiscard]] const WorldDescription& description() const noexcept;

        // Cold access creates a requested index once; const access requires that slot to be prepared.
        // The returned index has a stable address until World destruction. Geometry is already qualified.
        [[nodiscard]] WorldSpatialIndex& spatialIndex(std::uint32_t geometry) noexcept;
        [[nodiscard]] const WorldSpatialIndex& spatialIndex(std::uint32_t geometry) const noexcept;

        // Persistent identities are independent of EnTT slots. Mutate only at the Engine structural safe point.
        [[nodiscard]] ecs::WorldEntityMap& identities() noexcept;
        [[nodiscard]] const ecs::WorldEntityMap& identities() const noexcept;

    private:
        void onIdentityCreated(ecs::Registry& registry, ecs::Entity entity) noexcept;
        void onIdentityUpdated(ecs::Registry& registry, ecs::Entity entity) noexcept;
        void onIdentityDestroyed(ecs::Registry& registry, ecs::Entity entity) noexcept;

        WorldDescription description_;
        ecs::Registry registry_;
        ecs::WorldEntityMap identities_;
        std::vector<std::unique_ptr<WorldSpatialIndex>> spatial_indices_;
        // Persistent identities are stable until component removal; observers only maintain the external index.
        std::array<entt::scoped_connection, 3> identity_connections_;
    };
} // namespace lux::world
