#pragma once

#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/persistence/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <uuid.h>

#include <cstdint>
#include <vector>

namespace lux::ecs
{
    struct PersistentEntityId final
    {
        uuids::uuid value;

        [[nodiscard]] bool operator==(
            const PersistentEntityId& other
        ) const noexcept = default;
    };

    struct PersistentEntityRef final
    {
        PersistentEntityId value;
    };

    struct PersistentId final
    {
        PersistentEntityId value;
    };

    enum class EPersistentEntityIndexError : std::uint8_t
    {
        DUPLICATE_ID,
        INVALID_ID,
        ALLOCATION_FAILURE,
    };

    class LUX_ENGINE_ECS_PERSISTENCE_PUBLIC PersistentEntityIndex final
    {
      public:
        [[nodiscard]] static lux::cxx::expected<
            PersistentEntityIndex,
            EPersistentEntityIndexError>
        build(const World& world) noexcept;

        [[nodiscard]] Entity find(PersistentEntityId id) const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;

      private:
        std::vector<std::pair<PersistentEntityId, Entity>> entries_;
    };

    [[nodiscard]] LUX_ENGINE_ECS_PERSISTENCE_PUBLIC ComponentSchema
    persistentIdComponentSchema();
} // namespace lux::ecs
