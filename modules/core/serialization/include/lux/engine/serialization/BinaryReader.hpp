#pragma once

#include <lux/engine/serialization/BinaryWriter.hpp>

#include <bit>
#include <cstring>
#include <limits>
#include <span>

namespace lux::serialization
{
    class BinaryReader final
    {
    public:
        explicit BinaryReader(
            std::span<const std::byte> source,
            SerializationLimits limits = {}
        ) noexcept
            : source_(source), limits_(limits)
        {
        }

        [[nodiscard]] std::size_t offset() const noexcept
        {
            return offset_;
        }

        [[nodiscard]] std::size_t remaining() const noexcept
        {
            return source_.size() - offset_;
        }

        [[nodiscard]] const SerializationLimits& limits() const noexcept
        {
            return limits_;
        }

        [[nodiscard]] SerializationResult readBytes(
            std::span<std::byte> destination
        ) noexcept
        {
            if (destination.size() > remaining())
            {
                return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{
                    ESerializationError::TRUNCATED,
                    offset_
                });
            }
            std::memcpy(
                destination.data(),
                source_.data() + offset_,
                destination.size()
            );
            offset_ += destination.size();
            return {};
        }

        template <std::unsigned_integral T>
        [[nodiscard]] lux::cxx::expected<T, SerializationFailure>
        readUnsigned() noexcept
        {
            if (sizeof(T) > remaining())
            {
                return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{
                    ESerializationError::TRUNCATED,
                    offset_
                });
            }
            T value{};
            for (std::size_t index{}; index < sizeof(T); ++index)
            {
                value |= static_cast<T>(
                    std::to_integer<std::uint8_t>(source_[offset_ + index])
                ) << (index * 8U);
            }
            offset_ += sizeof(T);
            return value;
        }

        template <std::signed_integral T>
        [[nodiscard]] lux::cxx::expected<T, SerializationFailure>
        readSigned() noexcept
        {
            auto value = readUnsigned<std::make_unsigned_t<T>>();
            if (!value)
            {
                return lux::cxx::unexpected<SerializationFailure>(value.error());
            }
            return static_cast<T>(*value);
        }

        template <std::floating_point T>
        [[nodiscard]] lux::cxx::expected<T, SerializationFailure>
        readFloat() noexcept
        {
            if constexpr (sizeof(T) == sizeof(std::uint32_t))
            {
                auto bits = readUnsigned<std::uint32_t>();
                if (!bits)
                {
                    return lux::cxx::unexpected<SerializationFailure>(bits.error());
                }
                return std::bit_cast<T>(*bits);
            }
            else
            {
                static_assert(sizeof(T) == sizeof(std::uint64_t));
                auto bits = readUnsigned<std::uint64_t>();
                if (!bits)
                {
                    return lux::cxx::unexpected<SerializationFailure>(bits.error());
                }
                return std::bit_cast<T>(*bits);
            }
        }

    private:
        std::span<const std::byte> source_;
        SerializationLimits limits_;
        std::size_t offset_{};
    };
} // namespace lux::serialization
