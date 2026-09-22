#pragma once
#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/simulation/ecs/ComponentSchema.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>

namespace lux::simulation::ecs
{
// Adds only Entity's durable identity semantics. Primitive values, reflected fields and nested
// containers still use the existing binary serialization implementation and its budgets.
template <class Binary>
    requires(std::same_as<Binary, serialization::BinaryReader> || std::same_as<Binary, serialization::BinaryWriter>)
class WorldComponentArchive final
{
  public:
    WorldComponentArchive(Binary &binary, const WorldEntityMap &identities, const Registry &registry) noexcept
        : binary_(binary), identities_(&identities), registry_(&registry)
    {
    }

    WorldComponentArchive(Binary &binary, ComponentEntityResolver resolver) noexcept
        requires std::same_as<Binary, serialization::BinaryReader>
        : binary_(binary), resolver_(resolver)
    {
    }

    // A frozen identity map is sufficient when encoding an owned capture on another lane.
    // It is captured at the same structural safe point as the component values.
    WorldComponentArchive(Binary &binary, const WorldEntityMap &captured_identities) noexcept
        requires std::same_as<Binary, serialization::BinaryWriter>
        : binary_(binary), identities_(&captured_identities)
    {
    }

    [[nodiscard]] std::size_t offset() const noexcept
    {
        return binary_.offset();
    }
    [[nodiscard]] world::WorldObjectId unresolvedReference() const noexcept
    {
        return unresolved_;
    }
    [[nodiscard]] Entity invalidReference() const noexcept
    {
        return invalid_;
    }
    [[nodiscard]] auto remaining() const noexcept
        requires std::same_as<Binary, serialization::BinaryReader>
    {
        return binary_.remaining();
    }
    [[nodiscard]] auto writeBytes(std::span<const std::byte> bytes) noexcept
        requires std::same_as<Binary, serialization::BinaryWriter>
    {
        return binary_.writeBytes(bytes);
    }
    [[nodiscard]] auto readBytes(std::span<std::byte> bytes) noexcept
        requires std::same_as<Binary, serialization::BinaryReader>
    {
        return binary_.readBytes(bytes);
    }
    template <std::unsigned_integral T> [[nodiscard]] auto writeUnsigned(T value) noexcept
    {
        return binary_.writeUnsigned(value);
    }
    template <std::signed_integral T> [[nodiscard]] auto writeSigned(T value) noexcept
    {
        return binary_.writeSigned(value);
    }
    template <std::floating_point T> [[nodiscard]] auto writeFloat(T value) noexcept
    {
        return binary_.writeFloat(value);
    }
    template <std::unsigned_integral T> [[nodiscard]] auto readUnsigned() noexcept
    {
        return binary_.template readUnsigned<T>();
    }
    template <std::signed_integral T> [[nodiscard]] auto readSigned() noexcept
    {
        return binary_.template readSigned<T>();
    }
    template <std::floating_point T> [[nodiscard]] auto readFloat() noexcept
    {
        return binary_.template readFloat<T>();
    }

    [[nodiscard]] serialization::SerializationResult writeEntity(Entity value) noexcept
        requires std::same_as<Binary, serialization::BinaryWriter>
    {
        const auto identity = identities_->object(value);
        if (value != NullEntity && (!identity.valid() || (registry_ && !registry_->valid(value))))
        {
            invalid_ = value;
            return lux::cxx::unexpected(
                serialization::SerializationFailure{serialization::ESerializationError::INVALID_VALUE, offset()});
        }
        return binary_.writeBytes(identity.value.as_bytes());
    }
    [[nodiscard]] serialization::SerializationResult readEntity(Entity &value) noexcept
        requires std::same_as<Binary, serialization::BinaryReader>
    {
        const auto start = offset();
        std::array<std::uint8_t, 16> bytes{};
        auto read = binary_.readBytes(std::as_writable_bytes(std::span(bytes)));
        if (!read)
        {
            return read;
        }
        const world::WorldObjectId identity{uuids::uuid(bytes)};
        if (!identity.valid())
        {
            value = NullEntity;
            return {};
        }
        const auto resolved =
            resolver_.resolve ? resolver_.resolve(resolver_.state, identity) : resolveCurrent(identity);
        if (!resolved)
        {
            unresolved_ = identity;
            return lux::cxx::unexpected(
                serialization::SerializationFailure{serialization::ESerializationError::INVALID_VALUE, start});
        }
        value = *resolved;
        return {};
    }

  private:
    Binary &binary_;
    [[nodiscard]] lux::cxx::expected<Entity, ComponentDecodeFailure> resolveCurrent(
        world::WorldObjectId identity) const noexcept
    {
        const auto entity = identities_->entity(identity);
        if (entity == NullEntity || !registry_->valid(entity))
        {
            return lux::cxx::unexpected(
                ComponentDecodeFailure{EComponentDecodeError::UNRESOLVED_REFERENCE, offset(), identity});
        }
        return entity;
    }

    const WorldEntityMap *identities_{};
    const Registry *registry_{};
    ComponentEntityResolver resolver_{};
    world::WorldObjectId unresolved_;
    Entity invalid_{NullEntity};
};
} // namespace lux::simulation::ecs

namespace lux::serialization
{
template <> struct Serializer<simulation::ecs::Entity> final
{
    static constexpr EWireExtent wire_extent = EWireExtent::FIXED;
    static constexpr std::size_t fixed_wire_size = 16;
    template <class Writer>
        requires requires(Writer &writer, simulation::ecs::Entity value) { writer.writeEntity(value); }
    static SerializationResult write(Writer &writer, simulation::ecs::Entity value,
                                     const SerializationContext &) noexcept
    {
        return writer.writeEntity(value);
    }
    static SerializationResult read(simulation::ecs::WorldComponentArchive<BinaryReader> &reader,
                                    simulation::ecs::Entity &value, const SerializationContext &) noexcept
    {
        return reader.readEntity(value);
    }
};
} // namespace lux::serialization
