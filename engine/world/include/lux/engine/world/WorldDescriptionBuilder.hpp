#pragma once

#include <lux/engine/world/WorldDescription.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace lux::world
{
    enum class EWorldDescriptionError : std::uint8_t
    {
        INVALID_OBJECT_ID,
        DUPLICATE_OBJECT_ID,
        OBJECT_NOT_FOUND,
        INVALID_SCHEMA_ID,
        SCHEMA_HASH_COLLISION,
        INVALID_SCHEMA_VERSION,
        DUPLICATE_OBJECT_DATA,
        DATA_NOT_FOUND,
        SIZE_OVERFLOW,
        ALLOCATION_FAILURE,
    };

    struct WorldDescriptionFailure final
    {
        EWorldDescriptionError code{EWorldDescriptionError::ALLOCATION_FAILURE};
        WorldObjectId object;
        WorldDataSchemaId schema;
    };

    class LUX_ENGINE_WORLD_PUBLIC WorldDescriptionBuilder final
    {
      public:
        WorldDescriptionBuilder();
        ~WorldDescriptionBuilder();
        WorldDescriptionBuilder(WorldDescriptionBuilder&&) noexcept;
        WorldDescriptionBuilder& operator=(WorldDescriptionBuilder&&) noexcept;

        WorldDescriptionBuilder(const WorldDescriptionBuilder&) = delete;
        WorldDescriptionBuilder& operator=(const WorldDescriptionBuilder&) = delete;

        [[nodiscard]] lux::cxx::expected<void, WorldDescriptionFailure>
        addObject(WorldObjectId id) noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldDescriptionFailure>
        eraseObject(WorldObjectId id) noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldDescriptionFailure>
        addData(
            WorldObjectId object,
            WorldDataSchemaId schema,
            std::uint32_t version,
            std::span<const std::byte> payload
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<void, WorldDescriptionFailure>
        setData(
            WorldObjectId object,
            WorldDataSchemaId schema,
            std::uint32_t version,
            std::span<const std::byte> payload
        ) noexcept;
        
        [[nodiscard]] lux::cxx::expected<void, WorldDescriptionFailure>
        eraseData(
            WorldObjectId object,
            const WorldDataSchemaId& schema
        ) noexcept;
        void clear() noexcept;

        [[nodiscard]] lux::cxx::expected<
            WorldDescription,
            WorldDescriptionFailure>
        build() && noexcept;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
