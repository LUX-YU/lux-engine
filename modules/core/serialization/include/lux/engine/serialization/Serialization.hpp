#pragma once

#include <lux/engine/meta/TypeStaticInfo.hpp>
#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/serialization/Traits.hpp>
#include <lux/engine/serialization/visibility.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::serialization
{
    [[nodiscard]] LUX_CORE_SERIALIZATION_PUBLIC
    std::uint32_t binarySerializationContractVersion() noexcept;

    namespace detail
    {
        template <class T>
        struct IsVector : std::false_type {};

        template <class T, class Allocator>
        struct IsVector<std::vector<T, Allocator>> : std::true_type {};

        template <class T>
        struct IsOptional : std::false_type {};

        template <class T>
        struct IsOptional<std::optional<T>> : std::true_type {};

        template <class T>
        struct IsArray : std::false_type {};

        template <class T, std::size_t Size>
        struct IsArray<std::array<T, Size>> : std::true_type {};

        template <class T>
        struct IsPair : std::false_type {};

        template <class First, class Second>
        struct IsPair<std::pair<First, Second>> : std::true_type {};

        template <class T, class = void>
        struct IsTupleLike : std::false_type {};

        template <class T>
        struct IsTupleLike<T, std::void_t<decltype(std::tuple_size<T>::value)>>
            : std::true_type {};

        template <class Writer, class T>
        concept HasCustomWrite = requires(Writer& writer, const T& value)
        {
            { Serializer<T>::write(writer, value) } ->
                std::same_as<SerializationResult>;
        };

        template <class Reader, class T>
        concept HasCustomRead = requires(Reader& reader, T& value)
        {
            { Serializer<T>::read(reader, value) } ->
                std::same_as<SerializationResult>;
        };

        template <class T>
        concept SemanticArchiveOnly = requires(T value)
        {
            luxBinarySemanticArchiveOnly(value);
        };

        template <class Writer, class T>
        [[nodiscard]] SerializationResult writeValue(
            Writer& writer,
            const T& value,
            std::uint32_t depth
        ) noexcept;

        template <class Reader, class T>
        [[nodiscard]] SerializationResult readValue(
            Reader& reader,
            T& value,
            std::uint32_t depth
        ) noexcept;

        template <class Writer, class Tuple, std::size_t... Indices>
        [[nodiscard]] SerializationResult writeTuple(
            Writer& writer,
            const Tuple& tuple,
            std::uint32_t depth,
            std::index_sequence<Indices...>
        ) noexcept
        {
            SerializationResult result{};
            const auto write_one = [&](const auto& item)
            {
                if (result)
                {
                    result = writeValue(writer, item, depth + 1U);
                }
            };
            (write_one(std::get<Indices>(tuple)), ...);
            return result;
        }

        template <class Reader, class Tuple, std::size_t... Indices>
        [[nodiscard]] SerializationResult readTuple(
            Reader& reader,
            Tuple& tuple,
            std::uint32_t depth,
            std::index_sequence<Indices...>
        ) noexcept
        {
            SerializationResult result{};
            const auto read_one = [&](auto& item)
            {
                if (result)
                {
                    result = readValue(reader, item, depth + 1U);
                }
            };
            (read_one(std::get<Indices>(tuple)), ...);
            return result;
        }

        template <class Writer, class T>
        [[nodiscard]] SerializationResult writeValue(
            Writer& writer,
            const T& value,
            std::uint32_t depth
        ) noexcept
        {
            using U = std::remove_cvref_t<T>;
            if (depth > writer.limits().max_nesting)
            {
                return lux::cxx::unexpected<SerializationFailure>(
                    SerializationFailure{
                        ESerializationError::LIMIT_EXCEEDED,
                        writer.offset()
                    }
                );
            }
            if constexpr (HasSerializerDefinition<U>)
            {
                if constexpr (HasCustomWrite<Writer, U>)
                {
                    try
                    {
                        return Serializer<U>::write(writer, value);
                    }
                    catch (const std::bad_alloc&)
                    {
                        return lux::cxx::unexpected<SerializationFailure>(
                            SerializationFailure{
                                ESerializationError::ALLOCATION_FAILURE,
                                writer.offset()
                            }
                        );
                    }
                    catch (...)
                    {
                        return lux::cxx::unexpected<SerializationFailure>(
                            SerializationFailure{
                                ESerializationError::INVALID_VALUE,
                                writer.offset()
                            }
                        );
                    }
                }
                else
                {
                    static_assert(
                        HasCustomWrite<Writer, U>,
                        "Serializer<T> exists but does not support this Writer"
                    );
                }
            }
            else if constexpr (std::same_as<U, bool>)
            {
                return writer.template writeUnsigned<std::uint8_t>(
                    value ? 1U : 0U
                );
            }
            else if constexpr (std::unsigned_integral<U>)
            {
                return writer.writeUnsigned(value);
            }
            else if constexpr (std::signed_integral<U>)
            {
                return writer.writeSigned(value);
            }
            else if constexpr (std::floating_point<U>)
            {
                return writer.writeFloat(value);
            }
            else if constexpr (SemanticArchiveOnly<U>)
            {
                static_assert(
                    !SemanticArchiveOnly<U>,
                    "Type requires an ECS-aware semantic binary archive"
                );
            }
            else if constexpr (std::is_enum_v<U>)
            {
                using Underlying = std::underlying_type_t<U>;
                return writeValue(writer, static_cast<Underlying>(value), depth);
            }
            else if constexpr (std::same_as<U, std::string>)
            {
                if (value.size() > writer.limits().max_string_bytes)
                {
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{
                            ESerializationError::LIMIT_EXCEEDED,
                            writer.offset()
                        }
                    );
                }
                auto result = writer.template writeUnsigned<std::uint64_t>(
                    value.size()
                );
                if (!result)
                {
                    return result;
                }
                return writer.writeBytes(std::as_bytes(std::span(value)));
            }
            else if constexpr (IsVector<U>::value)
            {
                if (value.size() > writer.limits().max_container_elements)
                {
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{
                            ESerializationError::LIMIT_EXCEEDED,
                            writer.offset()
                        }
                    );
                }
                auto result = writer.template writeUnsigned<std::uint64_t>(
                    value.size()
                );
                for (const auto& item : value)
                {
                    if (!result)
                    {
                        return result;
                    }
                    result = writeValue(writer, item, depth + 1U);
                }
                return result;
            }
            else if constexpr (IsOptional<U>::value)
            {
                auto result = writer.template writeUnsigned<std::uint8_t>(
                    value ? 1U : 0U
                );
                if (!result || !value)
                {
                    return result;
                }
                return writeValue(writer, *value, depth + 1U);
            }
            else if constexpr (lux::meta::HasTypeStaticInfo<U>)
            {
                SerializationResult result{};
                std::apply(
                    [&](const auto&... field)
                    {
                        const auto write_field = [&](const auto& descriptor)
                        {
                            if (result)
                            {
                                result = writeValue(
                                    writer,
                                    value.*descriptor.pointer,
                                    depth + 1U
                                );
                            }
                        };
                        (write_field(field), ...);
                    },
                    lux::meta::TypeStaticInfo<U>::fields
                );
                return result;
            }
            else if constexpr (IsTupleLike<U>::value)
            {
                return writeTuple(
                    writer,
                    value,
                    depth,
                    std::make_index_sequence<std::tuple_size_v<U>>{}
                );
            }
            else
            {
                static_assert(sizeof(U) == 0, "Type is not binary serializable");
            }
        }

        template <class Reader, class T>
        [[nodiscard]] SerializationResult readValue(
            Reader& reader,
            T& value,
            std::uint32_t depth
        ) noexcept
        {
            using U = std::remove_cvref_t<T>;
            if (depth > reader.limits().max_nesting)
            {
                return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{
                    ESerializationError::LIMIT_EXCEEDED,
                    reader.offset()
                });
            }
            if constexpr (HasSerializerDefinition<U>)
            {
                if constexpr (HasCustomRead<Reader, U>)
                {
                    try
                    {
                        return Serializer<U>::read(reader, value);
                    }
                    catch (const std::bad_alloc&)
                    {
                        return lux::cxx::unexpected<SerializationFailure>(
                            SerializationFailure{
                                ESerializationError::ALLOCATION_FAILURE,
                                reader.offset()
                            }
                        );
                    }
                    catch (...)
                    {
                        return lux::cxx::unexpected<SerializationFailure>(
                            SerializationFailure{
                                ESerializationError::INVALID_VALUE,
                                reader.offset()
                            }
                        );
                    }
                }
                else
                {
                    static_assert(
                        HasCustomRead<Reader, U>,
                        "Serializer<T> exists but does not support this Reader"
                    );
                }
            }
            else if constexpr (std::same_as<U, bool>)
            {
                auto encoded = reader.template readUnsigned<std::uint8_t>();
                if (!encoded)
                {
                    return lux::cxx::unexpected<SerializationFailure>(encoded.error());
                }
                if (*encoded > 1U)
                {
                    return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{
                        ESerializationError::INVALID_VALUE,
                        reader.offset() - 1U
                    });
                }
                value = *encoded != 0U;
                return {};
            }
            else if constexpr (std::unsigned_integral<U>)
            {
                auto encoded = reader.template readUnsigned<U>();
                if (!encoded)
                {
                    return lux::cxx::unexpected<SerializationFailure>(encoded.error());
                }
                value = *encoded;
                return {};
            }
            else if constexpr (std::signed_integral<U>)
            {
                auto encoded = reader.template readSigned<U>();
                if (!encoded)
                {
                    return lux::cxx::unexpected<SerializationFailure>(encoded.error());
                }
                value = *encoded;
                return {};
            }
            else if constexpr (std::floating_point<U>)
            {
                auto encoded = reader.template readFloat<U>();
                if (!encoded)
                {
                    return lux::cxx::unexpected<SerializationFailure>(encoded.error());
                }
                value = *encoded;
                return {};
            }
            else if constexpr (SemanticArchiveOnly<U>)
            {
                static_assert(
                    !SemanticArchiveOnly<U>,
                    "Type requires an ECS-aware semantic binary archive"
                );
            }
            else if constexpr (std::is_enum_v<U>)
            {
                std::underlying_type_t<U> encoded{};
                auto result = readValue(reader, encoded, depth);
                if (result)
                {
                    value = static_cast<U>(encoded);
                }
                return result;
            }
            else if constexpr (std::same_as<U, std::string>)
            {
                auto size = reader.template readUnsigned<std::uint64_t>();
                if (!size)
                {
                    return lux::cxx::unexpected<SerializationFailure>(size.error());
                }
                if (*size > reader.limits().max_string_bytes ||
                    *size > std::numeric_limits<std::size_t>::max())
                {
                    return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{
                        ESerializationError::LIMIT_EXCEEDED,
                        reader.offset()
                    });
                }
                try
                {
                    value.resize(static_cast<std::size_t>(*size));
                }
                catch (const std::bad_alloc&)
                {
                    return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{
                        ESerializationError::ALLOCATION_FAILURE,
                        reader.offset()
                    });
                }
                catch (...)
                {
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{
                            ESerializationError::LIMIT_EXCEEDED,
                            reader.offset()
                        }
                    );
                }
                return reader.readBytes(std::as_writable_bytes(std::span(value)));
            }
            else if constexpr (IsVector<U>::value)
            {
                auto size = reader.template readUnsigned<std::uint64_t>();
                if (!size)
                {
                    return lux::cxx::unexpected<SerializationFailure>(size.error());
                }
                if (*size > reader.limits().max_container_elements ||
                    *size > std::numeric_limits<std::size_t>::max())
                {
                    return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{
                        ESerializationError::LIMIT_EXCEEDED,
                        reader.offset()
                    });
                }
                try
                {
                    value.resize(static_cast<std::size_t>(*size));
                }
                catch (const std::bad_alloc&)
                {
                    return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{
                        ESerializationError::ALLOCATION_FAILURE,
                        reader.offset()
                    });
                }
                catch (...)
                {
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{
                            ESerializationError::LIMIT_EXCEEDED,
                            reader.offset()
                        }
                    );
                }
                SerializationResult result{};
                for (auto& item : value)
                {
                    if (!result)
                    {
                        return result;
                    }
                    result = readValue(reader, item, depth + 1U);
                }
                return result;
            }
            else if constexpr (IsOptional<U>::value)
            {
                auto present = reader.template readUnsigned<std::uint8_t>();
                if (!present)
                {
                    return lux::cxx::unexpected<SerializationFailure>(present.error());
                }
                if (*present > 1U)
                {
                    return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{
                        ESerializationError::INVALID_VALUE,
                        reader.offset() - 1U
                    });
                }
                if (*present == 0U)
                {
                    value.reset();
                    return {};
                }
                value.emplace();
                return readValue(reader, *value, depth + 1U);
            }
            else if constexpr (lux::meta::HasTypeStaticInfo<U>)
            {
                SerializationResult result{};
                std::apply(
                    [&](const auto&... field)
                    {
                        const auto read_field = [&](const auto& descriptor)
                        {
                            if (result)
                            {
                                result = readValue(
                                    reader,
                                    value.*descriptor.pointer,
                                    depth + 1U
                                );
                            }
                        };
                        (read_field(field), ...);
                    },
                    lux::meta::TypeStaticInfo<U>::fields
                );
                return result;
            }
            else if constexpr (IsTupleLike<U>::value)
            {
                return readTuple(
                    reader,
                    value,
                    depth,
                    std::make_index_sequence<std::tuple_size_v<U>>{}
                );
            }
            else
            {
                static_assert(sizeof(U) == 0, "Type is not binary serializable");
            }
        }
    } // namespace detail

    template <class Writer, class T>
    [[nodiscard]] SerializationResult write(
        Writer& writer,
        const T& value
    ) noexcept
    {
        return detail::writeValue(writer, value, 0U);
    }

    template <class Reader, class T>
    [[nodiscard]] SerializationResult read(Reader& reader, T& value) noexcept
    {
        return detail::readValue(reader, value, 0U);
    }

    template <class T, class Reader>
        requires std::default_initializable<T>
    [[nodiscard]] lux::cxx::expected<T, SerializationFailure>
    read(Reader& reader) noexcept
    {
        try
        {
            T value{};
            auto result = read(reader, value);
            if (!result)
            {
                return lux::cxx::unexpected<SerializationFailure>(result.error());
            }
            return value;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected<SerializationFailure>(
                SerializationFailure{
                    ESerializationError::ALLOCATION_FAILURE,
                    reader.offset()
                }
            );
        }
        catch (...)
        {
            return lux::cxx::unexpected<SerializationFailure>(
                SerializationFailure{
                    ESerializationError::INVALID_VALUE,
                    reader.offset()
                }
            );
        }
    }

    template <class T>
    concept Serializable = requires(BinaryWriter& writer, BinaryReader& reader, T& value)
    {
        { write(writer, std::as_const(value)) } -> std::same_as<SerializationResult>;
        { read(reader, value) } -> std::same_as<SerializationResult>;
    };
} // namespace lux::serialization
