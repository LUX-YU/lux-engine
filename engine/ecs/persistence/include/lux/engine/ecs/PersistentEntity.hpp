#pragma once

#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/ComponentAnnotations.hpp>
#include <lux/engine/ecs/ComponentLoadBinding.hpp>
#include <lux/engine/ecs/ComponentSnapshotBinding.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/persistence/visibility.h>
#include <lux/engine/meta/MetaAnnotations.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <uuid.h>

#include <cstdint>
#include <vector>

namespace lux::ecs
{
    struct LUX_TYPE_INFO(static) PersistentEntityId final
    {
        uuids::uuid value;

        [[nodiscard]] bool operator==(
            const PersistentEntityId& other
        ) const noexcept = default;
    };

    struct LUX_TYPE_INFO(static) PersistentEntityRef final
    {
        PersistentEntityId value;
    };

    struct LUX_COMPONENT(
        schema = "lux.ecs.PersistentId",
        version = 1,
        snapshot = COPY,
        section = LOAD
    ) PersistentId final
    {
        PersistentEntityId value;
    };

    enum class EPersistentEntityIndexError : std::uint8_t
    {
        WORLD_BUSY,
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

    [[nodiscard]] LUX_ENGINE_ECS_PERSISTENCE_PUBLIC
    const ComponentSchema& persistentIdComponentSchema() noexcept;

    [[nodiscard]] LUX_ENGINE_ECS_PERSISTENCE_PUBLIC
    ComponentLoadContribution persistentEntityComponentLoadContribution() noexcept;

    [[nodiscard]] LUX_ENGINE_ECS_PERSISTENCE_PUBLIC
    ComponentSnapshotContribution
    persistentEntityComponentSnapshotContribution() noexcept;
} // namespace lux::ecs

#if !defined(__LUX_PARSE_TIME__)
#    include <lux/engine/ecs/PersistentEntity.type_static_info.hpp>
#endif
