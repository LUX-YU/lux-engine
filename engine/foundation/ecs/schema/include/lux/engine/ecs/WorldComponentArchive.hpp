#pragma once
#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/Registry.hpp>
#include <lux/engine/ecs/WorldEntityMap.hpp>

namespace lux::ecs
{
    // Adds only Entity's durable identity semantics. Primitive values, reflected fields and nested
    // containers still use the existing binary serialization implementation and its budgets.
    template <class Binary>
        requires(std::same_as<Binary, serialization::BinaryReader> || std::same_as<Binary, serialization::BinaryWriter>)
    class TWorldComponentArchive final
    {
    public:
        TWorldComponentArchive(Binary& binary, const WorldEntityMap& identities, const Registry& registry) noexcept
            requires std::same_as<Binary, serialization::BinaryReader>
            : binary_(binary), identities_(&identities), registry_(&registry)
        {}

        TWorldComponentArchive(Binary& binary, ComponentEntityResolver resolver) noexcept
            requires std::same_as<Binary, serialization::BinaryReader>
            : binary_(binary), resolver_(resolver)
        {}

        // Encode author data at its structural safe point. No live Registry borrow escapes to Worker IO.
        TWorldComponentArchive(Binary& binary, const Registry& registry) noexcept
            requires std::same_as<Binary, serialization::BinaryWriter>
            : binary_(binary), registry_(&registry)
        {}

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
            world::WorldObjectId identity{};
            if (value != NullEntity)
            {
                const auto* stored = registry_->valid(value) ? registry_->try_get<world::WorldObjectId>(value) : nullptr;
                const bool is_invalid_reference = !stored || !world::isValid(*stored);
                if (is_invalid_reference)
                {
                    invalid_ = value;
                    return lux::cxx::unexpected(serialization::SerializationFailure{
                        serialization::ESerializationError::INVALID_VALUE, offset()
                    });
                }
                identity = *stored;
            }
            return binary_.writeBytes(identity.value.as_bytes());
        }
        [[nodiscard]] serialization::SerializationResult readEntity(Entity& value) noexcept
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
            if (!world::isValid(identity))
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
                    serialization::SerializationFailure{serialization::ESerializationError::INVALID_VALUE, start}
                );
            }
            value = *resolved;
            return {};
        }

    private:
        Binary& binary_;
        [[nodiscard]] lux::cxx::expected<Entity, ComponentDecodeFailure> resolveCurrent(world::WorldObjectId identity
        ) const noexcept
        {
            const auto entity = identities_->entity(identity);
            if (entity == NullEntity || !registry_->valid(entity))
            {
                return lux::cxx::unexpected(
                    ComponentDecodeFailure{EComponentDecodeError::UNRESOLVED_REFERENCE, offset(), identity}
                );
            }
            return entity;
        }

        const WorldEntityMap* identities_{};
        const Registry* registry_{};
        ComponentEntityResolver resolver_{};
        world::WorldObjectId unresolved_;
        Entity invalid_{NullEntity};
    };
} // namespace lux::ecs

namespace lux::serialization
{
    template <> struct TSerializer<ecs::Entity> final
    {
        static constexpr EWireExtent wire_extent = EWireExtent::FIXED;
        static constexpr std::size_t fixed_wire_size = 16;
        template <class Writer>
            requires requires(Writer& writer, ecs::Entity value) { writer.writeEntity(value); }
        static SerializationResult
        write(Writer& writer, ecs::Entity value, const SerializationContext&) noexcept
        {
            return writer.writeEntity(value);
        }
        static SerializationResult
        read(
            ecs::TWorldComponentArchive<BinaryReader>& reader,
            ecs::Entity& value,
            const SerializationContext&
        ) noexcept
        {
            return reader.readEntity(value);
        }
    };
} // namespace lux::serialization
