#pragma once

#include <lux/engine/serialization/Serialization.hpp>

#include <lux/cxx/compile_time/TypeToken.hpp>

#include <cstddef>
#include <span>
#include <type_traits>
#include <vector>

namespace lux::serialization
{
    inline constexpr SerializationBudget kPortableMetadataBudget{
        1U << 20U,
        1U << 16U,
        32U
    };

    using EncodeDefaultPortableValueFn = SerializationResult (*)(std::vector<std::byte>& output) noexcept;
    using EncodePortableValueFn = SerializationResult (*)(
        const void* object,
        std::vector<std::byte>& output
    ) noexcept;
    using DecodePortableValueFn = SerializationResult (*)(
        std::span<const std::byte> input,
        void* object
    ) noexcept;

    struct PortableValueCodec final
    {
        lux::cxx::TypeToken type{};
        EncodeDefaultPortableValueFn encode_default{};
        EncodePortableValueFn encode{};
        DecodePortableValueFn decode{};

        [[nodiscard]] bool valid() const noexcept
        {
            return type.isValid() && encode_default != nullptr && encode != nullptr && decode != nullptr;
        }
    };

    template <class Value>
    [[nodiscard]] PortableValueCodec makePortableValueCodec() noexcept
    {
        static_assert(std::is_nothrow_default_constructible_v<Value>);
        static_assert(std::is_nothrow_destructible_v<Value>);

        return PortableValueCodec{
            .type = lux::cxx::typeToken<Value>(),
            .encode_default = +[](std::vector<std::byte>& output) noexcept -> SerializationResult {
                Value value{};
                output.clear();
                BinaryWriter writer(output);
                return serialization::write(writer, value, kPortableMetadataBudget);
            },
            .encode = +[](const void* object, std::vector<std::byte>& output) noexcept -> SerializationResult {
                if (object == nullptr)
                {
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{ESerializationError::INVALID_VALUE, 0U}
                    );
                }
                output.clear();
                BinaryWriter writer(output);
                return serialization::write(
                    writer,
                    *static_cast<const Value*>(object),
                    kPortableMetadataBudget
                );
            },
            .decode = +[](std::span<const std::byte> input, void* object) noexcept -> SerializationResult {
                if (object == nullptr)
                {
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{ESerializationError::INVALID_VALUE, 0U}
                    );
                }
                BinaryReader reader(input);
                auto result = serialization::read(
                    reader,
                    *static_cast<Value*>(object),
                    kPortableMetadataBudget
                );
                if (!result)
                {
                    return result;
                }
                if (reader.remaining() != 0U)
                {
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{ESerializationError::INVALID_VALUE, reader.offset()}
                    );
                }
                return {};
            }
        };
    }

    [[nodiscard]] constexpr PortableValueCodec noPortableValueCodec() noexcept
    {
        return {};
    }
} // namespace lux::serialization
