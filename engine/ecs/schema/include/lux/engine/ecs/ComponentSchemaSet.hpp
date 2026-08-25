#pragma once

#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/schema/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace lux::ecs
{
    enum class ESchemaError : std::uint8_t
    {
        INVALID_SCHEMA_ID,
        INVALID_CPP_TYPE,
        INVALID_VERSION,
        INVALID_OPERATIONS,
        COPY_WITHOUT_CLONE,
        DUPLICATE_SCHEMA_ID,
        SCHEMA_ID_COLLISION,
        DUPLICATE_CPP_TYPE,
        CPP_TYPE_COLLISION,
        ALLOCATION_FAILURE,
    };

    struct SchemaFailure final
    {
        ESchemaError code{ESchemaError::INVALID_SCHEMA_ID};
        ComponentSchemaId schema;
        lux::cxx::TypeToken cpp_type;
    };

    class LUX_ENGINE_ECS_SCHEMA_PUBLIC ComponentSchemaSet final
    {
      public:
        ComponentSchemaSet() noexcept = default;

        [[nodiscard]] static lux::cxx::expected<ComponentSchemaSet, SchemaFailure>
        build(std::vector<ComponentSchema> schemas) noexcept;

        [[nodiscard]] static lux::cxx::expected<ComponentSchemaSet, SchemaFailure>
        build(
            std::span<const ComponentSchema> schemas,
            std::shared_ptr<const void> code_lifetime
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<ComponentSchemaSet, SchemaFailure>
        extended(std::span<const ComponentSchema> schemas) const noexcept;

        [[nodiscard]] lux::cxx::expected<ComponentSchemaSet, SchemaFailure>
        extended(
            std::span<const ComponentSchema> schemas,
            std::shared_ptr<const void> code_lifetime
        ) const noexcept;

        [[nodiscard]] const ComponentSchema* find(
            const ComponentSchemaId& id
        ) const noexcept;

        [[nodiscard]] const ComponentSchema* find(
            lux::cxx::TypeToken type
        ) const noexcept;

        [[nodiscard]] std::span<const ComponentSchema> all() const noexcept;
        [[nodiscard]] bool empty() const noexcept;

      private:
        struct Impl;
        explicit ComponentSchemaSet(std::shared_ptr<const Impl> impl) noexcept;

        std::shared_ptr<const Impl> impl_;
    };
} // namespace lux::ecs
