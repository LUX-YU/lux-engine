#pragma once

#include <lux/engine/serialization/BinaryWriter.hpp>
#include <lux/engine/simulation/ecs/ComponentOperations.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaId.hpp>
#include <lux/engine/simulation/ecs/DecodedComponent.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/world/WorldObjectId.hpp>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace lux::simulation::ecs
{
class WorldEntityMap;
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
    UNRESOLVED_REFERENCE,
};

struct ComponentDecodeFailure final
{
    EComponentDecodeError code{EComponentDecodeError::MALFORMED_PAYLOAD};
    std::size_t offset{};
    world::WorldObjectId reference;
};

using DecodeEmplaceComponentFn = lux::cxx::expected<void, ComponentDecodeFailure> (*)(
    Registry &registry, const WorldEntityMap &identities, Entity entity, std::uint32_t encoded_schema_version,
    std::span<const std::byte> encoded_payload) noexcept;

using ComponentEncodeResult = lux::cxx::expected<std::vector<std::byte>, serialization::SerializationFailure>;

// Immutable typed content and its defining code survive independently of the source Registry.
class ComponentCapture final
{
  public:
    using EncodeFn = ComponentEncodeResult (*)(const void *, const WorldEntityMap &, std::size_t);

    ComponentCapture(std::shared_ptr<const void> value, EncodeFn encode) : value_(std::move(value)), encode_(encode)
    {
    }

    [[nodiscard]] ComponentEncodeResult encode(const WorldEntityMap &identities, std::size_t limit) const
    {
        return encode_(value_.get(), identities, limit);
    }

  private:
    std::shared_ptr<const void> value_;
    EncodeFn encode_;
};

struct ComponentEntityResolver final
{
    const void *state;
    lux::cxx::expected<Entity, ComponentDecodeFailure> (*resolve)(const void *, world::WorldObjectId) noexcept;
};

using DecodeComponentValueFn = lux::cxx::expected<DecodedComponent, ComponentDecodeFailure> (*)(
    std::uint32_t, std::span<const std::byte>, ComponentEntityResolver, std::shared_ptr<const void>);

using CaptureComponentFn = lux::cxx::expected<ComponentCapture, ComponentDecodeFailure> (*)(
    const Registry &, Entity, std::shared_ptr<const void>);
using CaptureComponentValueFn = ComponentCapture (*)(const void *, std::shared_ptr<const void>);

struct ComponentEntityVisitor final
{
    void *state;
    void (*visit)(void *, Entity) noexcept;
};
// Read-only traversal through the component's author codec; no byte buffer or copy.
// Missing/failed traversal means reference safety cannot be established.
using VisitComponentReferencesFn = serialization::SerializationResult (*)(const Registry &, Entity,
                                                                          ComponentEntityVisitor) noexcept;

struct ComponentSchema final
{
    lux::cxx::TypeToken cpp_type;
    ComponentSchemaId id;
    std::uint32_t version{1};
    ComponentOperations operations;
    DecodeEmplaceComponentFn decode_emplace{};
    EComponentSnapshotPolicy snapshot{EComponentSnapshotPolicy::COPY};
    EComponentSemanticKind semantic_kind{EComponentSemanticKind::DOMAIN_CONTRACT};
    std::shared_ptr<const void> code_lifetime;
    CaptureComponentFn capture{};
    DecodeComponentValueFn decode_value{};
    CaptureComponentValueFn capture_value{};
    VisitComponentReferencesFn visit_references{};
};

template <class Component>
[[nodiscard]] ComponentSchema makeComponentSchema(
    ComponentSchemaId id, std::uint32_t version, EComponentSnapshotPolicy snapshot,
    std::shared_ptr<const void> code_lifetime, DecodeEmplaceComponentFn decode_emplace,
    EComponentSemanticKind semantic_kind, CaptureComponentFn capture = nullptr,
    DecodeComponentValueFn decode_value = nullptr, CaptureComponentValueFn capture_value = nullptr,
    VisitComponentReferencesFn visit_references = nullptr)
{
    return ComponentSchema{lux::cxx::typeToken<Component>(),
                           std::move(id),
                           version,
                           componentOperations<Component>(),
                           decode_emplace,
                           snapshot,
                           semantic_kind,
                           std::move(code_lifetime),
                           capture,
                           decode_value,
                           capture_value,
                           visit_references};
}
} // namespace lux::simulation::ecs
