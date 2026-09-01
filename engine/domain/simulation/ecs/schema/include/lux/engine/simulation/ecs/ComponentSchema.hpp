#pragma once

#include <lux/engine/simulation/ecs/ComponentOperations.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaId.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace lux::simulation::ecs
{
    enum class EComponentSnapshotPolicy : std::uint8_t
    {
        COPY,
        REBUILD,
    };

    enum class EComponentSemanticKind : std::uint8_t
    {
        FOUNDATION,
        DOMAIN_CONTRACT,
        IMPLEMENTATION_EXTENSION,
        RUNTIME_DERIVED,
    };

    enum class EComponentDecodeError : std::uint8_t
    {
        INVALID_ENTITY,
        UNSUPPORTED_VERSION,
        MALFORMED_PAYLOAD,
        UNSUPPORTED_TYPE,
        COMPONENT_CONSTRUCTION_FAILURE,
        ALLOCATION_FAILURE,
    };

    struct ComponentDecodeFailure final
    {
        EComponentDecodeError code{EComponentDecodeError::MALFORMED_PAYLOAD};
        std::size_t offset{};
    };

    using DecodeEmplaceComponentFn = lux::cxx::expected<void, ComponentDecodeFailure> (*)(
        Registry& registry,
        Entity entity,
        std::uint32_t encoded_schema_version,
        std::span<const std::byte> encoded_payload
    ) noexcept;

    struct ComponentSchema final
    {
        lux::cxx::TypeToken cpp_type;
        ComponentSchemaId id;
        std::uint32_t version{1};
        ComponentOperations operations;
        DecodeEmplaceComponentFn decode_emplace{};
        EComponentSnapshotPolicy snapshot{EComponentSnapshotPolicy::COPY};
        EComponentSemanticKind semantic_kind{EComponentSemanticKind::DOMAIN_CONTRACT};
        bool editor_visible{true};
        std::shared_ptr<const void> code_lifetime;
    };

    template <class Component>
    [[nodiscard]] ComponentSchema makeComponentSchema(
        ComponentSchemaId id,
        std::uint32_t version,
        EComponentSnapshotPolicy snapshot,
        std::shared_ptr<const void> code_lifetime,
        DecodeEmplaceComponentFn decode_emplace,
        EComponentSemanticKind semantic_kind,
        bool editor_visible
    )
    {
        return ComponentSchema{
            lux::cxx::typeToken<Component>(),
            std::move(id),
            version,
            componentOperations<Component>(),
            decode_emplace,
            snapshot,
            semantic_kind,
            editor_visible,
            std::move(code_lifetime)};
    }
} // namespace lux::simulation::ecs
