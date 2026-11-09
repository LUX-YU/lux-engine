#pragma once

#include <lux/engine/services/serialization/scene/visibility.h>
#include <lux/engine/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/ecs/WorldEntityMap.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace lux::scene
{
    enum class EWorldMaterializeError : std::uint8_t
    {
        INVALID_WORLD_SCHEMA,
        INVALID_OBJECT,
        COMPONENT_DECODE_FAILURE,
        ALLOCATION_FAILURE,
        DUPLICATE_OBJECT,
        CAPACITY,
        STRUCTURE_CHANGED,
    };

    struct WorldMaterializeFailure final
    {
        EWorldMaterializeError code{EWorldMaterializeError::INVALID_OBJECT};
        ecs::ComponentDecodeFailure component;
        std::size_t object{};
        std::size_t data{};
    };

    struct WorldComponentInput final
    {
        const ecs::ComponentSchema* schema{};
        std::uint32_t version{};
        std::span<const std::byte> payload;
    };

    struct WorldObjectInput final
    {
        world::WorldObjectId id;
        std::span<const WorldComponentInput> components;
    };

    class LUX_ENGINE_SCENE_ASSET_PUBLIC WorldMaterializer final
    {
    public:
        class LUX_ENGINE_SCENE_ASSET_PUBLIC PreparedObjects final
        {
        public:
            ~PreparedObjects() noexcept;
            PreparedObjects(PreparedObjects&&) noexcept;
            PreparedObjects& operator=(PreparedObjects&&) noexcept;
            [[nodiscard]] std::span<const ecs::Entity> entities() const noexcept;
            [[nodiscard]] std::span<const ecs::Entity> references() const noexcept;
            [[nodiscard]] ecs::Entity entity(world::WorldObjectId) const noexcept;
            [[nodiscard]] std::size_t componentBytes() const noexcept;
            // Same structural exclusion as prepare; no normal business rejection after mutation starts.
            void commit(ecs::Registry&, ecs::WorldEntityMap&) noexcept;

        private:
            friend class WorldMaterializer;
            struct Impl;
            explicit PreparedObjects(std::unique_ptr<Impl>) noexcept;
            std::unique_ptr<Impl> impl_;
        };

        [[nodiscard]] static lux::cxx::expected<PreparedObjects, WorldMaterializeFailure> prepareObjects(
            const ecs::Registry&,
            const ecs::WorldEntityMap&,
            std::span<const WorldObjectInput>,
            std::size_t maximum_component_bytes = (std::numeric_limits<std::size_t>::max)()
        ) noexcept;

    };
} // namespace lux::scene
