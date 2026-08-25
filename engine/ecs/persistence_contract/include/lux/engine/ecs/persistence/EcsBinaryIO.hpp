#pragma once

#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/serialization/Serialization.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace lux::ecs
{
    struct EntityOrdinal final
    {
        Entity entity{NullEntity};
        std::uint32_t ordinal{};
    };

    class EcsBinaryWriter final
    {
    public:
        EcsBinaryWriter(
            std::vector<std::byte>& destination,
            std::span<const EntityOrdinal> entity_ordinals,
            std::vector<std::uint64_t>* row_offsets = nullptr
        ) noexcept
            : writer_(destination),
              entity_ordinals_(entity_ordinals),
              row_offsets_(row_offsets)
        {
        }

        [[nodiscard]] lux::serialization::SerializationResult beginRow() noexcept
        {
            if (row_offsets_ == nullptr)
            {
                return {};
            }
            try
            {
                row_offsets_->push_back(writer_.offset());
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected<lux::serialization::SerializationFailure>(lux::serialization::SerializationFailure{
                    lux::serialization::ESerializationError::ALLOCATION_FAILURE,
                    writer_.offset()
                });
            }
        }

        [[nodiscard]] lux::serialization::SerializationResult endColumn() noexcept
        {
            if (row_offsets_ == nullptr)
            {
                return {};
            }
            try
            {
                row_offsets_->push_back(writer_.offset());
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected<lux::serialization::SerializationFailure>(lux::serialization::SerializationFailure{
                    lux::serialization::ESerializationError::ALLOCATION_FAILURE,
                    writer_.offset()
                });
            }
        }

        [[nodiscard]] std::size_t offset() const noexcept { return writer_.offset(); }
        [[nodiscard]] const lux::serialization::SerializationLimits& limits() const noexcept
        {
            return writer_.limits();
        }

        [[nodiscard]] lux::serialization::SerializationResult writeBytes(
            std::span<const std::byte> bytes
        ) noexcept
        {
            return writer_.writeBytes(bytes);
        }

        template <std::unsigned_integral T>
        [[nodiscard]] lux::serialization::SerializationResult writeUnsigned(
            T value
        ) noexcept
        {
            return writer_.writeUnsigned(value);
        }

        template <std::signed_integral T>
        [[nodiscard]] lux::serialization::SerializationResult writeSigned(
            T value
        ) noexcept
        {
            return writer_.writeSigned(value);
        }

        template <std::floating_point T>
        [[nodiscard]] lux::serialization::SerializationResult writeFloat(
            T value
        ) noexcept
        {
            return writer_.writeFloat(value);
        }

        [[nodiscard]] lux::serialization::SerializationResult
        writeEntityReference(Entity entity) noexcept
        {
            const auto iterator = std::lower_bound(
                entity_ordinals_.begin(),
                entity_ordinals_.end(),
                entity,
                [](const EntityOrdinal& item, Entity value)
                {
                    return static_cast<std::uint32_t>(item.entity) <
                        static_cast<std::uint32_t>(value);
                }
            );
            if (iterator == entity_ordinals_.end() || iterator->entity != entity)
            {
                return lux::cxx::unexpected<lux::serialization::SerializationFailure>(lux::serialization::SerializationFailure{
                    lux::serialization::ESerializationError::INVALID_VALUE,
                    writer_.offset()
                });
            }
            return writer_.writeUnsigned(iterator->ordinal);
        }

    private:
        lux::serialization::BinaryWriter writer_;
        std::span<const EntityOrdinal> entity_ordinals_;
        std::vector<std::uint64_t>* row_offsets_{};
    };

    class EcsBinaryReader final
    {
    public:
        EcsBinaryReader(
            std::span<const std::byte> payload,
            std::span<const std::uint64_t> row_offsets,
            bool fixed_width,
            std::uint64_t fixed_stride,
            std::span<const Entity> ordinal_entities,
            lux::serialization::SerializationLimits limits = {}
        ) noexcept
            : payload_(payload),
              row_offsets_(row_offsets),
              fixed_width_(fixed_width),
              fixed_stride_(fixed_stride),
              ordinal_entities_(ordinal_entities),
              limits_(limits),
              reader_({}, limits)
        {
        }

        [[nodiscard]] lux::serialization::SerializationResult beginRow(
            std::size_t row
        ) noexcept
        {
            std::uint64_t begin{};
            std::uint64_t end{};
            if (fixed_width_)
            {
                if (fixed_stride_ != 0U &&
                    row > std::numeric_limits<std::uint64_t>::max() /
                        fixed_stride_)
                {
                    return invalid();
                }
                begin = row * fixed_stride_;
                end = begin + fixed_stride_;
            }
            else
            {
                if (row + 1U >= row_offsets_.size())
                {
                    return invalid();
                }
                begin = row_offsets_[row];
                end = row_offsets_[row + 1U];
            }
            if (begin > end || end > payload_.size())
            {
                return invalid();
            }
            reader_ = lux::serialization::BinaryReader(
                payload_.subspan(
                    static_cast<std::size_t>(begin),
                    static_cast<std::size_t>(end - begin)
                ),
                limits_
            );
            return {};
        }

        [[nodiscard]] lux::serialization::SerializationResult endRow() const noexcept
        {
            if (reader_.remaining() != 0U)
            {
                return invalid();
            }
            return {};
        }

        [[nodiscard]] std::size_t offset() const noexcept { return reader_.offset(); }
        [[nodiscard]] const lux::serialization::SerializationLimits& limits() const noexcept
        {
            return limits_;
        }
        [[nodiscard]] lux::serialization::SerializationResult readBytes(
            std::span<std::byte> bytes
        ) noexcept
        {
            return reader_.readBytes(bytes);
        }
        template <std::unsigned_integral T>
        [[nodiscard]] auto readUnsigned() noexcept { return reader_.readUnsigned<T>(); }
        template <std::signed_integral T>
        [[nodiscard]] auto readSigned() noexcept { return reader_.readSigned<T>(); }
        template <std::floating_point T>
        [[nodiscard]] auto readFloat() noexcept { return reader_.readFloat<T>(); }

        [[nodiscard]] lux::cxx::expected<Entity, lux::serialization::SerializationFailure>
        readEntityReference() noexcept
        {
            auto ordinal = reader_.readUnsigned<std::uint32_t>();
            if (!ordinal)
            {
                return lux::cxx::unexpected<lux::serialization::SerializationFailure>(ordinal.error());
            }
            if (*ordinal >= ordinal_entities_.size())
            {
                return lux::cxx::unexpected<lux::serialization::SerializationFailure>(lux::serialization::SerializationFailure{
                    lux::serialization::ESerializationError::INVALID_VALUE,
                    reader_.offset()
                });
            }
            return ordinal_entities_[*ordinal];
        }

    private:
        [[nodiscard]] lux::serialization::SerializationResult invalid() const noexcept
        {
            return lux::cxx::unexpected<lux::serialization::SerializationFailure>(lux::serialization::SerializationFailure{
                lux::serialization::ESerializationError::INVALID_VALUE,
                reader_.offset()
            });
        }

        std::span<const std::byte> payload_;
        std::span<const std::uint64_t> row_offsets_;
        bool fixed_width_{};
        std::uint64_t fixed_stride_{};
        std::span<const Entity> ordinal_entities_;
        lux::serialization::SerializationLimits limits_;
        lux::serialization::BinaryReader reader_;
    };
} // namespace lux::ecs

namespace lux::serialization
{
    template <>
    struct Serializer<lux::ecs::Entity>
    {
        [[nodiscard]] static SerializationResult write(
            lux::ecs::EcsBinaryWriter& writer,
            lux::ecs::Entity entity
        ) noexcept
        {
            return writer.writeEntityReference(entity);
        }

        [[nodiscard]] static SerializationResult read(
            lux::ecs::EcsBinaryReader& reader,
            lux::ecs::Entity& entity
        ) noexcept
        {
            auto result = reader.readEntityReference();
            if (!result)
            {
                return lux::cxx::unexpected<SerializationFailure>(result.error());
            }
            entity = *result;
            return {};
        }
    };
} // namespace lux::serialization
