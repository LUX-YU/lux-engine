#pragma once

#include <lux/engine/serialization/SerializationError.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace lux::serialization
{
    using SerializationResult =
        lux::cxx::expected<void, SerializationFailure>;

    class BinaryWriter final
    {
    public:
        explicit BinaryWriter(std::vector<std::byte>& destination) noexcept
            : destination_(&destination)
        {
        }

        [[nodiscard]] std::size_t offset() const noexcept
        {
            return destination_->size();
        }

        [[nodiscard]] SerializationResult writeBytes(
            std::span<const std::byte> bytes
        ) noexcept
        {
            try
            {
                destination_->insert(
                    destination_->end(),
                    bytes.begin(),
                    bytes.end()
                );
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{
                    ESerializationError::ALLOCATION_FAILURE,
                    offset()
                });
            }
        }

        template <std::unsigned_integral T>
        [[nodiscard]] SerializationResult writeUnsigned(T value) noexcept
        {
            std::byte bytes[sizeof(T)];
            for (std::size_t index{}; index < sizeof(T); ++index)
            {
                bytes[index] = static_cast<std::byte>(value & T{0xFFU});
                if constexpr (sizeof(T) > 1U)
                {
                    if (index + 1U < sizeof(T))
                        value = static_cast<T>(value >> 8U);
                }
            }
            return writeBytes(bytes);
        }

        template <std::signed_integral T>
        [[nodiscard]] SerializationResult writeSigned(T value) noexcept
        {
            return writeUnsigned(static_cast<std::make_unsigned_t<T>>(value));
        }

        template <std::floating_point T>
        [[nodiscard]] SerializationResult writeFloat(T value) noexcept
        {
            if constexpr (sizeof(T) == sizeof(std::uint32_t))
            {
                return writeUnsigned(std::bit_cast<std::uint32_t>(value));
            }
            else
            {
                static_assert(sizeof(T) == sizeof(std::uint64_t));
                return writeUnsigned(std::bit_cast<std::uint64_t>(value));
            }
        }

    private:
        std::vector<std::byte>* destination_{};
    };
} // namespace lux::serialization
