#pragma once

#include <lux/engine/ecs/World.hpp>

#include <uuid.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lux::ecs::persistence::detail
{
    template <class Integer>
        requires std::is_integral_v<Integer> && std::is_unsigned_v<Integer>
    void appendLittle(std::vector<std::byte>& destination, Integer value)
    {
        const std::size_t offset = destination.size();
        destination.resize(offset + sizeof(Integer));
        for (std::size_t index{}; index < sizeof(Integer); ++index)
        {
            destination[offset + index] = static_cast<std::byte>(
                value & static_cast<Integer>(0xffU)
            );
            value >>= 8U;
        }
    }

    template <class Integer>
        requires std::is_integral_v<Integer> && std::is_unsigned_v<Integer>
    [[nodiscard]] bool readLittle(
        std::span<const std::byte> bytes,
        std::size_t& offset,
        Integer& value
    ) noexcept
    {
        if (offset > bytes.size() || sizeof(Integer) > bytes.size() - offset)
            return false;
        value = 0;
        for (std::size_t index{}; index < sizeof(Integer); ++index)
        {
            value |= static_cast<Integer>(
                std::to_integer<unsigned char>(bytes[offset + index])
            ) << (index * 8U);
        }
        offset += sizeof(Integer);
        return true;
    }

    inline void patchU32(
        std::vector<std::byte>& destination,
        std::size_t offset,
        std::uint32_t value
    ) noexcept
    {
        lux::ecs::detail::require(
            offset <= destination.size() &&
            sizeof(value) <= destination.size() - offset
        );
        for (std::size_t index{}; index < sizeof(value); ++index)
        {
            destination[offset + index] = static_cast<std::byte>(
                value & 0xffU
            );
            value >>= 8U;
        }
    }

    class Writer final
    {
      public:
        explicit Writer(std::vector<std::byte>& destination) noexcept
            : destination_(&destination)
        {
        }

        void writeBytes(const void* data, std::size_t size)
        {
            if (size == 0)
                return;
            const auto* first = static_cast<const std::byte*>(data);
            destination_->insert(destination_->end(), first, first + size);
        }

        template <class Integer>
            requires std::is_integral_v<Integer> && std::is_unsigned_v<Integer>
        void writeUnsigned(Integer value)
        {
            appendLittle(*destination_, value);
        }

        void writeString(std::string_view value)
        {
            lux::ecs::detail::require(
                value.size() <= static_cast<std::size_t>(UINT32_MAX)
            );
            writeUnsigned(static_cast<std::uint32_t>(value.size()));
            writeBytes(value.data(), value.size());
        }

        void writeUuid(const uuids::uuid& value)
        {
            const auto bytes = value.as_bytes();
            writeBytes(bytes.data(), bytes.size());
        }

      private:
        std::vector<std::byte>* destination_{};
    };

    class Reader final
    {
      public:
        explicit Reader(std::span<const std::byte> bytes) noexcept
            : bytes_(bytes)
        {
        }

        void readBytes(void* destination, std::size_t size) noexcept
        {
            if (!valid_ || offset_ > bytes_.size() ||
                size > bytes_.size() - offset_)
            {
                valid_ = false;
                return;
            }
            if (size != 0)
            {
                std::memcpy(destination, bytes_.data() + offset_, size);
                offset_ += size;
            }
        }

        template <class Integer>
            requires std::is_integral_v<Integer> && std::is_unsigned_v<Integer>
        [[nodiscard]] Integer readUnsigned() noexcept
        {
            Integer value{};
            if (!valid_ || !readLittle(bytes_, offset_, value))
                valid_ = false;
            return value;
        }

        [[nodiscard]] uuids::uuid readUuid() noexcept
        {
            std::array<std::uint8_t, 16> bytes{};
            readBytes(bytes.data(), bytes.size());
            return uuids::uuid(bytes);
        }

        [[nodiscard]] std::span<const std::byte> readSpan(
            std::size_t size
        ) noexcept
        {
            if (!valid_ || offset_ > bytes_.size() ||
                size > bytes_.size() - offset_)
            {
                valid_ = false;
                return {};
            }
            const auto result = bytes_.subspan(offset_, size);
            offset_ += size;
            return result;
        }

        [[nodiscard]] bool ok() const noexcept { return valid_; }
        [[nodiscard]] bool eof() const noexcept { return offset_ == bytes_.size(); }

      private:
        std::span<const std::byte> bytes_;
        std::size_t offset_{};
        bool valid_{true};
    };
} // namespace lux::ecs::persistence::detail
