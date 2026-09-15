#pragma once

#include <lux/engine/meta/TypeStaticInfo.hpp>
#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/serialization/Traits.hpp>
#include <lux/engine/serialization/visibility.h>
#include <lux/cxx/core/EnumFlags.hpp>

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
    [[nodiscard]] LUX_CORE_SERIALIZATION_PUBLIC std::uint32_t binarySerializationContractVersion() noexcept;

    class SerializationContext final
    {
    public:
        explicit constexpr SerializationContext(const SerializationBudget& budget) noexcept
            : budget_(std::addressof(budget))
        {
        }

        [[nodiscard]] constexpr const SerializationBudget& budget() const noexcept
        {
            return *budget_;
        }

        [[nodiscard]] constexpr std::uint32_t depth() const noexcept
        {
            return depth_;
        }

        [[nodiscard]] constexpr SerializationContext nested() const noexcept
        {
            SerializationContext result(*budget_);
            result.depth_ = depth_ == std::numeric_limits<std::uint32_t>::max() ? depth_ : depth_ + 1U;
            return result;
        }

    private:
        const SerializationBudget* budget_{};
        std::uint32_t depth_{};
    };

    template <class Enum>
        requires std::is_enum_v<Enum>
    struct Serializer<lux::cxx::EnumFlags<Enum>> final
    {
        using Flags = lux::cxx::EnumFlags<Enum>;
        using Underlying = typename Flags::underlying_type;

        template <class Writer>
        static SerializationResult write(Writer& writer, Flags value, const SerializationContext&) noexcept
        {
            if constexpr (std::is_unsigned_v<Underlying>) return writer.writeUnsigned(value.bits());
            else return writer.writeSigned(value.bits());
        }

        template <class Reader>
        static SerializationResult read(Reader& reader, Flags& value, const SerializationContext&) noexcept
        {
            auto bits = [&]() {
                if constexpr (std::is_unsigned_v<Underlying>) return reader.template readUnsigned<Underlying>();
                else return reader.template readSigned<Underlying>();
            }();
            if (!bits) return lux::cxx::unexpected<SerializationFailure>(bits.error());
            value = Flags::fromBits(*bits);
            return {};
        }
    };

    namespace detail
    {
        template <class T> struct IsVector : std::false_type
        {
        };

        template <class T, class Allocator> struct IsVector<std::vector<T, Allocator>> : std::true_type
        {
        };

        template <class T> struct IsOptional : std::false_type
        {
        };

        template <class T> struct IsOptional<std::optional<T>> : std::true_type
        {
        };

        template <class T> struct IsArray : std::false_type
        {
        };

        template <class T, std::size_t Size> struct IsArray<std::array<T, Size>> : std::true_type
        {
        };

        template <class T> struct IsPair : std::false_type
        {
        };

        template <class First, class Second> struct IsPair<std::pair<First, Second>> : std::true_type
        {
        };

        template <class T, class = void> struct IsTupleLike : std::false_type
        {
        };

        template <class T> struct IsTupleLike<T, std::void_t<decltype(std::tuple_size<T>::value)>> : std::true_type
        {
        };

        template <class Writer, class T>
        concept HasCustomWrite = requires(Writer& writer, const T& value, const SerializationContext& context) {
            { Serializer<T>::write(writer, value, context) } -> std::same_as<SerializationResult>;
        };

        template <class Reader, class T>
        concept HasCustomRead = requires(Reader& reader, T& value, const SerializationContext& context) {
            { Serializer<T>::read(reader, value, context) } -> std::same_as<SerializationResult>;
        };

        template <class T>
        concept SemanticArchiveOnly = requires(T value) { luxBinarySemanticArchiveOnly(value); };

        template <class Writer, class T>
        [[nodiscard]] SerializationResult
        writeValue(Writer& writer, const T& value, const SerializationContext& context) noexcept;

        template <class Reader, class T>
        [[nodiscard]] SerializationResult
        readValue(Reader& reader, T& value, const SerializationContext& context) noexcept;

        template <class Reader, class Variant, std::size_t Index = 0>
        SerializationResult readAlternative(Reader& reader, Variant& value, std::uint32_t index,
                                            const SerializationContext& context) noexcept
        {
            if constexpr (Index == std::variant_size_v<Variant>)
            {
                return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{ESerializationError::INVALID_VALUE, reader.offset()});
            }
            else
            {
                if (index == Index)
                {
                    std::variant_alternative_t<Index, Variant> prepared{};
                    auto result = readValue(reader, prepared, context.nested());
                    if (result)
                    {
                        value.template emplace<Index>(std::move(prepared));
                    }
                    return result;
                }
                return readAlternative<Reader, Variant, Index + 1>(reader, value, index, context);
            }
        }

        template <class Container> Container emptyContainer(const Container& source)
        {
            if constexpr (requires { source.hash_function(); source.key_eq(); })
            {
                Container result(0, source.hash_function(), source.key_eq(), source.get_allocator());
                result.max_load_factor(source.max_load_factor());
                return result;
            }
            else if constexpr (requires { source.key_comp(); })
            {
                return Container(source.key_comp(), source.get_allocator());
            }
            else
            {
                return Container(source.get_allocator());
            }
        }

        template <class Writer, class Tuple, std::size_t... Indices>
        [[nodiscard]] SerializationResult writeTuple(
            Writer& writer,
            const Tuple& tuple,
            const SerializationContext& context,
            std::index_sequence<Indices...>
        ) noexcept
        {
            SerializationResult result{};
            const auto write_one = [&](const auto& item) {
                if (result)
                {
                    result = writeValue(writer, item, context.nested());
                }
            };
            (write_one(std::get<Indices>(tuple)), ...);
            return result;
        }

        template <class Reader, class Tuple, std::size_t... Indices>
        [[nodiscard]] SerializationResult readTuple(
            Reader& reader,
            Tuple& tuple,
            const SerializationContext& context,
            std::index_sequence<Indices...>
        ) noexcept
        {
            SerializationResult result{};
            const auto read_one = [&](auto& item) {
                if (result)
                {
                    result = readValue(reader, item, context.nested());
                }
            };
            (read_one(std::get<Indices>(tuple)), ...);
            return result;
        }

        template <class Writer, class T>
        [[nodiscard]] SerializationResult
        writeValue(Writer& writer, const T& value, const SerializationContext& context) noexcept
        {
            using U = std::remove_cvref_t<T>;
            if (context.depth() > context.budget().max_nesting)
            {
                return lux::cxx::unexpected<SerializationFailure>(
                    SerializationFailure{ESerializationError::LIMIT_EXCEEDED, writer.offset()}
                );
            }
            if constexpr (HasSerializerDefinition<U>)
            {
                if constexpr (HasCustomWrite<Writer, U>)
                {
                    try
                    {
                        return Serializer<U>::write(writer, value, context);
                    }
                    catch (const std::bad_alloc&)
                    {
                        return lux::cxx::unexpected<SerializationFailure>(
                            SerializationFailure{ESerializationError::ALLOCATION_FAILURE, writer.offset()}
                        );
                    }
                    catch (...)
                    {
                        return lux::cxx::unexpected<SerializationFailure>(
                            SerializationFailure{ESerializationError::INVALID_VALUE, writer.offset()}
                        );
                    }
                }
                else
                {
                    static_assert(HasCustomWrite<Writer, U>, "Serializer<T> exists but does not support this Writer");
                }
            }
            else if constexpr (std::same_as<U, bool>)
            {
                return writer.template writeUnsigned<std::uint8_t>(value ? 1U : 0U);
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
                static_assert(!SemanticArchiveOnly<U>, "Type requires an ECS-aware semantic binary archive");
            }
            else if constexpr (std::is_enum_v<U>)
            {
                using Underlying = std::underlying_type_t<U>;
                return writeValue(writer, static_cast<Underlying>(value), context);
            }
            else if constexpr (std::same_as<U, std::string>)
            {
                if (value.size() > context.budget().max_string_bytes)
                {
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{ESerializationError::LIMIT_EXCEEDED, writer.offset()}
                    );
                }
                auto result = writer.template writeUnsigned<std::uint64_t>(value.size());
                if (!result)
                {
                    return result;
                }
                return writer.writeBytes(std::as_bytes(std::span(value)));
            }
            else if constexpr (SequenceContainer<U> || MapContainer<U> || SetContainer<U>)
            {
                if (value.size() > context.budget().max_container_elements)
                {
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{ESerializationError::LIMIT_EXCEEDED, writer.offset()}
                    );
                }
                auto result = writer.template writeUnsigned<std::uint64_t>(value.size());
                for (const auto& item : value)
                {
                    if (!result)
                    {
                        return result;
                    }
                    if constexpr (SequenceContainer<U> && std::same_as<typename U::value_type, bool>)
                    {
                        result = writeValue(writer, static_cast<bool>(item), context.nested());
                    }
                    else
                    {
                        result = writeValue(writer, item, context.nested());
                    }
                }
                return result;
            }
            else if constexpr (std::same_as<U, std::monostate>)
            {
                return {};
            }
            else if constexpr (VariantValue<U>)
            {
                if (value.valueless_by_exception())
                {
                    return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{ESerializationError::INVALID_VALUE, writer.offset()});
                }
                auto result = writer.template writeUnsigned<std::uint32_t>(static_cast<std::uint32_t>(value.index()));
                if (!result)
                {
                    return result;
                }
                return std::visit([&](const auto& item) { return writeValue(writer, item, context.nested()); }, value);
            }
            else if constexpr (IsOptional<U>::value)
            {
                auto result = writer.template writeUnsigned<std::uint8_t>(value ? 1U : 0U);
                if (!result || !value)
                {
                    return result;
                }
                return writeValue(writer, *value, context.nested());
            }
            else if constexpr (std::is_bounded_array_v<U>)
            {
                SerializationResult result{};
                for (const auto& item : value)
                {
                    if (result) result = writeValue(writer, item, context.nested());
                }
                return result;
            }
            else if constexpr (lux::meta::HasTypeStaticInfo<U>)
            {
                SerializationResult result{};
                std::apply(
                    [&](const auto&... field) {
                        const auto write_field = [&](const auto& descriptor) {
                            if (result)
                            {
                                result = writeValue(writer, value.*descriptor.pointer, context.nested());
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
                return writeTuple(writer, value, context, std::make_index_sequence<std::tuple_size_v<U>>{});
            }
            else
            {
                static_assert(sizeof(U) == 0, "Type is not binary serializable");
            }
        }

        template <class Reader, class T>
        [[nodiscard]] SerializationResult
        readValue(Reader& reader, T& value, const SerializationContext& context) noexcept
        {
            using U = std::remove_cvref_t<T>;
            if (context.depth() > context.budget().max_nesting)
            {
                return lux::cxx::unexpected<SerializationFailure>(
                    SerializationFailure{ESerializationError::LIMIT_EXCEEDED, reader.offset()}
                );
            }
            if constexpr (HasSerializerDefinition<U>)
            {
                if constexpr (HasCustomRead<Reader, U>)
                {
                    try
                    {
                        return Serializer<U>::read(reader, value, context);
                    }
                    catch (const std::bad_alloc&)
                    {
                        return lux::cxx::unexpected<SerializationFailure>(
                            SerializationFailure{ESerializationError::ALLOCATION_FAILURE, reader.offset()}
                        );
                    }
                    catch (...)
                    {
                        return lux::cxx::unexpected<SerializationFailure>(
                            SerializationFailure{ESerializationError::INVALID_VALUE, reader.offset()}
                        );
                    }
                }
                else
                {
                    static_assert(HasCustomRead<Reader, U>, "Serializer<T> exists but does not support this Reader");
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
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{ESerializationError::INVALID_VALUE, reader.offset() - 1U}
                    );
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
                static_assert(!SemanticArchiveOnly<U>, "Type requires an ECS-aware semantic binary archive");
            }
            else if constexpr (std::is_enum_v<U>)
            {
                std::underlying_type_t<U> encoded{};
                auto result = readValue(reader, encoded, context);
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
                if (*size > context.budget().max_string_bytes || *size > std::numeric_limits<std::size_t>::max())
                {
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{ESerializationError::LIMIT_EXCEEDED, reader.offset()}
                    );
                }
                try
                {
                    value.resize(static_cast<std::size_t>(*size));
                }
                catch (const std::bad_alloc&)
                {
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{ESerializationError::ALLOCATION_FAILURE, reader.offset()}
                    );
                }
                catch (...)
                {
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{ESerializationError::LIMIT_EXCEEDED, reader.offset()}
                    );
                }
                return reader.readBytes(std::as_writable_bytes(std::span(value)));
            }
            else if constexpr (SequenceContainer<U> || MapContainer<U> || SetContainer<U>)
            {
                auto size = reader.template readUnsigned<std::uint64_t>();
                if (!size)
                {
                    return lux::cxx::unexpected<SerializationFailure>(size.error());
                }
                if (*size > context.budget().max_container_elements || *size > std::numeric_limits<std::size_t>::max())
                {
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{ESerializationError::LIMIT_EXCEEDED, reader.offset()}
                    );
                }
                auto prepared = emptyContainer(value);
                if (*size > prepared.max_size())
                {
                    return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{ESerializationError::LIMIT_EXCEEDED, reader.offset()});
                }
                if constexpr (requires { prepared.reserve(static_cast<std::size_t>(*size)); })
                {
                    prepared.reserve(static_cast<std::size_t>(*size));
                }
                for (std::uint64_t index{}; index < *size; ++index)
                {
                    if constexpr (MapContainer<U>)
                    {
                        std::pair<typename U::key_type, typename U::mapped_type> item;
                        auto decoded = readValue(reader, item, context.nested());
                        if (!decoded)
                        {
                            return decoded;
                        }
                        if (!prepared.try_emplace(std::move(item.first), std::move(item.second)).second)
                        {
                            return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{ESerializationError::INVALID_VALUE, reader.offset()});
                        }
                    }
                    else
                    {
                        typename U::value_type item{};
                        auto decoded = readValue(reader, item, context.nested());
                        if (!decoded)
                        {
                            return decoded;
                        }
                        if constexpr (SetContainer<U>)
                        {
                            if (!prepared.insert(std::move(item)).second)
                            {
                                return lux::cxx::unexpected<SerializationFailure>(SerializationFailure{ESerializationError::INVALID_VALUE, reader.offset()});
                            }
                        }
                        else
                        {
                            prepared.push_back(std::move(item));
                        }
                    }
                }
                value = std::move(prepared);
                return {};
            }
            else if constexpr (std::same_as<U, std::monostate>)
            {
                return {};
            }
            else if constexpr (VariantValue<U>)
            {
                auto index = reader.template readUnsigned<std::uint32_t>();
                if (!index)
                {
                    return lux::cxx::unexpected<SerializationFailure>(index.error());
                }
                return readAlternative(reader, value, *index, context);
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
                    return lux::cxx::unexpected<SerializationFailure>(
                        SerializationFailure{ESerializationError::INVALID_VALUE, reader.offset() - 1U}
                    );
                }
                if (*present == 0U)
                {
                    value.reset();
                    return {};
                }
                value.emplace();
                return readValue(reader, *value, context.nested());
            }
            else if constexpr (std::is_bounded_array_v<U>)
            {
                SerializationResult result{};
                for (auto& item : value)
                {
                    if (result) result = readValue(reader, item, context.nested());
                }
                return result;
            }
            else if constexpr (lux::meta::HasTypeStaticInfo<U>)
            {
                SerializationResult result{};
                std::apply(
                    [&](const auto&... field) {
                        const auto read_field = [&](const auto& descriptor) {
                            if (result)
                            {
                                result = readValue(reader, value.*descriptor.pointer, context.nested());
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
                return readTuple(reader, value, context, std::make_index_sequence<std::tuple_size_v<U>>{});
            }
            else
            {
                static_assert(sizeof(U) == 0, "Type is not binary serializable");
            }
        }
    } // namespace detail

    template <class Writer, class T>
    [[nodiscard]] SerializationResult write(Writer& writer, const T& value, const SerializationBudget& budget) noexcept
    {
        return detail::writeValue(writer, value, SerializationContext(budget));
    }

    template <class Writer, class T>
    [[nodiscard]] SerializationResult write(Writer& writer, const T& value, const SerializationContext& parent) noexcept
    {
        return detail::writeValue(writer, value, parent.nested());
    }

    template <class Reader, class T>
    [[nodiscard]] SerializationResult read(Reader& reader, T& value, const SerializationBudget& budget) noexcept
    {
        return detail::readValue(reader, value, SerializationContext(budget));
    }

    template <class Reader, class T>
    [[nodiscard]] SerializationResult read(Reader& reader, T& value, const SerializationContext& parent) noexcept
    {
        return detail::readValue(reader, value, parent.nested());
    }

    template <class T, class Reader>
        requires std::default_initializable<T>
    [[nodiscard]] lux::cxx::expected<T, SerializationFailure>
    read(Reader& reader, const SerializationBudget& budget) noexcept
    {
        try
        {
            T value{};
            auto result = read(reader, value, budget);
            if (!result)
            {
                return lux::cxx::unexpected<SerializationFailure>(result.error());
            }
            return value;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected<SerializationFailure>(
                SerializationFailure{ESerializationError::ALLOCATION_FAILURE, reader.offset()}
            );
        }
        catch (...)
        {
            return lux::cxx::unexpected<SerializationFailure>(
                SerializationFailure{ESerializationError::INVALID_VALUE, reader.offset()}
            );
        }
    }

    template <class T>
    concept Serializable =
        requires(BinaryWriter& writer, BinaryReader& reader, T& value, const SerializationBudget& budget) {
            { write(writer, std::as_const(value), budget) } -> std::same_as<SerializationResult>;
            { read(reader, value, budget) } -> std::same_as<SerializationResult>;
        };
} // namespace lux::serialization
