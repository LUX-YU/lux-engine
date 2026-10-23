#pragma once

#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/serialization/Traits.hpp>

// Contain the Windows COM spelling introduced by the UUID system generator.
#pragma push_macro("interface")
#include <uuid.h>
#pragma pop_macro("interface")

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lux::serialization
{
    template <> struct TSerializer<uuids::uuid>
    {
        static constexpr EWireExtent wire_extent = EWireExtent::FIXED;
        static constexpr std::size_t fixed_wire_size = 16U;
        template <class Writer>
        [[nodiscard]] static SerializationResult
        write(Writer& writer, const uuids::uuid& value, const SerializationContext&) noexcept
        {
            const auto bytes = value.as_bytes();
            return writer.writeBytes(std::as_bytes(std::span(bytes)));
        }

        template <class Reader>
        [[nodiscard]] static SerializationResult
        read(Reader& reader, uuids::uuid& value, const SerializationContext&) noexcept
        {
            std::array<std::uint8_t, 16> bytes{};
            auto result = reader.readBytes(std::as_writable_bytes(std::span(bytes)));
            if (result)
            {
                value = uuids::uuid(bytes);
            }
            return result;
        }
    };
} // namespace lux::serialization
