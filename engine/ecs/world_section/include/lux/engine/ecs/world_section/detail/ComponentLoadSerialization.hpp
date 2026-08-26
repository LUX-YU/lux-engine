#pragma once

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/serialization/Serialization.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace lux::ecs::detail
{
    class ComponentLoadReader final
    {
      public:
        ComponentLoadReader(
            std::span<const std::byte> row,
            std::span<const Entity> ordinal_entities,
            lux::serialization::SerializationLimits limits
        ) noexcept
            : reader_(row, limits),
              ordinal_entities_(ordinal_entities)
        {
        }

        [[nodiscard]] std::size_t offset() const noexcept
        {
            return reader_.offset();
        }

        [[nodiscard]] std::size_t remaining() const noexcept
        {
            return reader_.remaining();
        }

        [[nodiscard]] const lux::serialization::SerializationLimits&
        limits() const noexcept
        {
            return reader_.limits();
        }

        [[nodiscard]] lux::serialization::SerializationResult readBytes(
            std::span<std::byte> destination
        ) noexcept
        {
            return reader_.readBytes(destination);
        }

        template <std::unsigned_integral T>
        [[nodiscard]] auto readUnsigned() noexcept
        {
            return reader_.template readUnsigned<T>();
        }

        template <std::signed_integral T>
        [[nodiscard]] auto readSigned() noexcept
        {
            return reader_.template readSigned<T>();
        }

        template <std::floating_point T>
        [[nodiscard]] auto readFloat() noexcept
        {
            return reader_.template readFloat<T>();
        }

        [[nodiscard]] lux::cxx::expected<
            Entity,
            lux::serialization::SerializationFailure>
        readEntityReference() noexcept
        {
            auto ordinal = reader_.readUnsigned<std::uint32_t>();
            if (!ordinal)
            {
                return lux::cxx::unexpected<
                    lux::serialization::SerializationFailure>(
                    ordinal.error()
                );
            }
            if (*ordinal >= ordinal_entities_.size())
            {
                return lux::cxx::unexpected<
                    lux::serialization::SerializationFailure>(
                    lux::serialization::SerializationFailure{
                        lux::serialization::ESerializationError::INVALID_VALUE,
                        reader_.offset()
                    }
                );
            }
            return ordinal_entities_[*ordinal];
        }

      private:
        lux::serialization::BinaryReader reader_;
        std::span<const Entity> ordinal_entities_;
    };

    [[nodiscard]] inline std::uint32_t readColumnU32(
        std::span<const std::byte> bytes,
        std::size_t index
    ) noexcept
    {
        require(index <= bytes.size());
        require(sizeof(std::uint32_t) <= bytes.size() - index);
        std::uint32_t result{};
        for (std::size_t byte{}; byte < sizeof(result); ++byte)
        {
            result |= static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(bytes[index + byte])
            ) << (byte * 8U);
        }
        return result;
    }
} // namespace lux::ecs::detail

namespace lux::serialization
{
    template <>
    struct Serializer<lux::ecs::Entity>
    {
        static constexpr EWireExtent wire_extent = EWireExtent::FIXED;
        static constexpr std::size_t fixed_wire_size = sizeof(std::uint32_t);

        template <class Writer>
            requires requires(Writer& writer, lux::ecs::Entity entity)
            {
                { writer.writeEntityReference(entity) } ->
                    std::same_as<SerializationResult>;
            }
        [[nodiscard]] static SerializationResult write(
            Writer& writer,
            lux::ecs::Entity entity
        ) noexcept
        {
            return writer.writeEntityReference(entity);
        }

        template <class Reader>
            requires requires(Reader& reader)
            {
                { reader.readEntityReference() } -> std::same_as<
                    lux::cxx::expected<
                        lux::ecs::Entity,
                        SerializationFailure>>;
            }
        [[nodiscard]] static SerializationResult read(
            Reader& reader,
            lux::ecs::Entity& entity
        ) noexcept
        {
            auto result = reader.readEntityReference();
            if (!result)
                return lux::cxx::unexpected<SerializationFailure>(result.error());
            entity = *result;
            return {};
        }
    };
} // namespace lux::serialization
