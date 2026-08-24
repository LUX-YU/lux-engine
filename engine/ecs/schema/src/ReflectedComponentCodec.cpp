#include <lux/engine/ecs/ComponentSchema.hpp>

#include <lux/engine/meta/Meta.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lux::ecs
{
    namespace
    {
        [[nodiscard]] bool excluded(const lux::meta::RefField& field) noexcept
        {
            if (field.visibility != lux::meta::EVisibility::Public)
                return true;
            if (field.annotation_str == nullptr)
                return false;
            const std::string_view annotations{field.annotation_str};
            return annotations.find("luxref::property::skip") != std::string_view::npos;
        }

        [[nodiscard]] const void* fieldAddress(
            const lux::meta::RefField& field,
            const void* object
        ) noexcept
        {
            return static_cast<const std::byte*>(object) + field.offset;
        }

        [[nodiscard]] void* fieldAddress(
            const lux::meta::RefField& field,
            void* object
        ) noexcept
        {
            return static_cast<std::byte*>(object) + field.offset;
        }

        template <class Unsigned>
        [[nodiscard]] std::array<std::byte, sizeof(Unsigned)> littleEndian(
            Unsigned value
        ) noexcept
        {
            static_assert(std::is_unsigned_v<Unsigned>);
            std::array<std::byte, sizeof(Unsigned)> bytes{};
            for (std::size_t index{}; index < bytes.size(); ++index)
            {
                bytes[index] = static_cast<std::byte>(value & 0xffu);
                value >>= 8u;
            }
            return bytes;
        }

        template <class Unsigned>
        [[nodiscard]] bool readLittleEndian(
            std::span<const std::byte> bytes,
            Unsigned& value
        ) noexcept
        {
            static_assert(std::is_unsigned_v<Unsigned>);
            if (bytes.size() != sizeof(Unsigned))
                return false;
            value = 0u;
            for (std::size_t index{}; index < bytes.size(); ++index)
            {
                value |= static_cast<Unsigned>(std::to_integer<unsigned char>(bytes[index]))
                    << (index * 8u);
            }
            return true;
        }

        [[nodiscard]] EComponentWireType wireType(
            const lux::meta::RefField& field
        ) noexcept
        {
            if (field.type.hash == lux::cxx::type_hash<Entity>())
                return EComponentWireType::LOCAL_ENTITY;
            using lux::meta::EBaseType;
            const auto base = static_cast<EBaseType>(field.type.qtype.base);
            switch (base)
            {
                case EBaseType::Bool:
                case EBaseType::Uint8:
                case EBaseType::Uint16:
                case EBaseType::Uint32:
                case EBaseType::Uint64:
                    return EComponentWireType::UNSIGNED_INTEGER;
                case EBaseType::Int8:
                case EBaseType::Int16:
                case EBaseType::Int32:
                case EBaseType::Int64:
                    return EComponentWireType::SIGNED_INTEGER;
                case EBaseType::Float:
                case EBaseType::Double:
                    return EComponentWireType::FLOATING_POINT;
                case EBaseType::Record:
                    return field.type.hash == lux::cxx::type_hash<std::string>()
                        ? EComponentWireType::UTF8
                        : EComponentWireType::BYTES;
                default:
                    return EComponentWireType::BYTES;
            }
        }

        [[nodiscard]] lux::cxx::expected<void, EComponentCodecError>
        encodeObject(
            const lux::meta::RefClass& reflection,
            const void* object,
            std::string_view prefix,
            ComponentEncodePort& port
        )
        {
            using lux::meta::EBaseType;
            for (const auto& field : reflection.fields)
            {
                if (excluded(field))
                    continue;
                std::string name;
                if (prefix.empty())
                    name.assign(field.name);
                else
                {
                    name.reserve(prefix.size() + field.name.size() + 1u);
                    name.assign(prefix);
                    name.push_back('.');
                    name.append(field.name);
                }

                const void* value = fieldAddress(field, object);
                if (field.type.hash == lux::cxx::type_hash<Entity>())
                {
                    auto result = port.writeEntity(
                        name,
                        *static_cast<const Entity*>(value)
                    );
                    if (!result)
                        return result;
                    continue;
                }
                const auto base = static_cast<EBaseType>(field.type.qtype.base);
                if (base == EBaseType::Record &&
                    field.type.hash != lux::cxx::type_hash<std::string>())
                {
                    const auto* nested = static_cast<const lux::meta::RefClass*>(field.type.ptr);
                    if (nested == nullptr)
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    auto result = encodeObject(*nested, value, name, port);
                    if (!result)
                        return result;
                    continue;
                }

                std::array<std::byte, 8> storage{};
                std::span<const std::byte> encoded;
                switch (base)
                {
                    case EBaseType::Bool:
                        storage[0] = static_cast<std::byte>(*static_cast<const bool*>(value));
                        encoded = std::span{storage}.first(1u);
                        break;
                    case EBaseType::Uint8:
                        storage[0] = static_cast<std::byte>(*static_cast<const std::uint8_t*>(value));
                        encoded = std::span{storage}.first(1u);
                        break;
                    case EBaseType::Uint16:
                    {
                        const auto bytes = littleEndian(*static_cast<const std::uint16_t*>(value));
                        std::copy(bytes.begin(), bytes.end(), storage.begin());
                        encoded = std::span{storage}.first(bytes.size());
                        break;
                    }
                    case EBaseType::Uint32:
                    {
                        const auto bytes = littleEndian(*static_cast<const std::uint32_t*>(value));
                        std::copy(bytes.begin(), bytes.end(), storage.begin());
                        encoded = std::span{storage}.first(bytes.size());
                        break;
                    }
                    case EBaseType::Uint64:
                    {
                        const auto bytes = littleEndian(*static_cast<const std::uint64_t*>(value));
                        std::copy(bytes.begin(), bytes.end(), storage.begin());
                        encoded = storage;
                        break;
                    }
                    case EBaseType::Int8:
                        storage[0] = static_cast<std::byte>(
                            static_cast<std::uint8_t>(*static_cast<const std::int8_t*>(value))
                        );
                        encoded = std::span{storage}.first(1u);
                        break;
                    case EBaseType::Int16:
                    {
                        const auto bytes = littleEndian(
                            static_cast<std::uint16_t>(*static_cast<const std::int16_t*>(value))
                        );
                        std::copy(bytes.begin(), bytes.end(), storage.begin());
                        encoded = std::span{storage}.first(bytes.size());
                        break;
                    }
                    case EBaseType::Int32:
                    {
                        const auto bytes = littleEndian(
                            static_cast<std::uint32_t>(*static_cast<const std::int32_t*>(value))
                        );
                        std::copy(bytes.begin(), bytes.end(), storage.begin());
                        encoded = std::span{storage}.first(bytes.size());
                        break;
                    }
                    case EBaseType::Int64:
                    {
                        const auto bytes = littleEndian(
                            static_cast<std::uint64_t>(*static_cast<const std::int64_t*>(value))
                        );
                        std::copy(bytes.begin(), bytes.end(), storage.begin());
                        encoded = storage;
                        break;
                    }
                    case EBaseType::Float:
                    {
                        const float number = *static_cast<const float*>(value);
                        if (!std::isfinite(number))
                            return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                        const auto bytes = littleEndian(std::bit_cast<std::uint32_t>(number));
                        std::copy(bytes.begin(), bytes.end(), storage.begin());
                        encoded = std::span{storage}.first(bytes.size());
                        break;
                    }
                    case EBaseType::Double:
                    {
                        const double number = *static_cast<const double*>(value);
                        if (!std::isfinite(number))
                            return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                        const auto bytes = littleEndian(std::bit_cast<std::uint64_t>(number));
                        std::copy(bytes.begin(), bytes.end(), storage.begin());
                        encoded = storage;
                        break;
                    }
                    case EBaseType::Record:
                    {
                        const auto& string = *static_cast<const std::string*>(value);
                        encoded = std::as_bytes(std::span{string.data(), string.size()});
                        break;
                    }
                    default:
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                }

                auto result = port.write(name, wireType(field), encoded);
                if (!result)
                    return result;
            }
            return {};
        }

        [[nodiscard]] const lux::meta::RefField* findField(
            const lux::meta::RefClass& reflection,
            std::string_view name,
            std::string_view& remainder
        ) noexcept
        {
            const auto dot = name.find('.');
            const auto head = name.substr(0u, dot);
            remainder = dot == std::string_view::npos
                ? std::string_view{}
                : name.substr(dot + 1u);
            for (const auto& field : reflection.fields)
            {
                if (!excluded(field) && field.name == head)
                    return std::addressof(field);
            }
            return nullptr;
        }

        [[nodiscard]] lux::cxx::expected<void, EComponentCodecError>
        decodeLeaf(
            const lux::meta::RefField& field,
            void* destination,
            EComponentWireType type,
            std::span<const std::byte> bytes,
            const ComponentDecodePort& port
        ) noexcept
        {
            if (field.type.hash == lux::cxx::type_hash<Entity>())
            {
                if (type != EComponentWireType::LOCAL_ENTITY)
                    return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                auto resolved = port.resolveEntity(bytes);
                if (!resolved)
                    return lux::cxx::unexpected(resolved.error());
                *static_cast<Entity*>(destination) = *resolved;
                return {};
            }
            using lux::meta::EBaseType;
            const auto base = static_cast<EBaseType>(field.type.qtype.base);
            if (type != wireType(field))
                return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);

            switch (base)
            {
                case EBaseType::Bool:
                    if (bytes.size() != 1u || std::to_integer<unsigned char>(bytes[0]) > 1u)
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    *static_cast<bool*>(destination) = bytes[0] != std::byte{};
                    return {};
                case EBaseType::Uint8:
                    if (bytes.size() != 1u)
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    *static_cast<std::uint8_t*>(destination) =
                        std::to_integer<std::uint8_t>(bytes[0]);
                    return {};
                case EBaseType::Int8:
                    if (bytes.size() != 1u)
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    *static_cast<std::int8_t*>(destination) = static_cast<std::int8_t>(
                        std::to_integer<std::uint8_t>(bytes[0])
                    );
                    return {};
                case EBaseType::Uint16:
                {
                    std::uint16_t value{};
                    if (!readLittleEndian(bytes, value))
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    *static_cast<std::uint16_t*>(destination) = value;
                    return {};
                }
                case EBaseType::Int16:
                {
                    std::uint16_t value{};
                    if (!readLittleEndian(bytes, value))
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    *static_cast<std::int16_t*>(destination) = static_cast<std::int16_t>(value);
                    return {};
                }
                case EBaseType::Uint32:
                case EBaseType::Int32:
                case EBaseType::Float:
                {
                    std::uint32_t value{};
                    if (!readLittleEndian(bytes, value))
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    if (base == EBaseType::Uint32)
                        *static_cast<std::uint32_t*>(destination) = value;
                    else if (base == EBaseType::Int32)
                        *static_cast<std::int32_t*>(destination) = static_cast<std::int32_t>(value);
                    else
                    {
                        const float number = std::bit_cast<float>(value);
                        if (!std::isfinite(number))
                            return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                        *static_cast<float*>(destination) = number;
                    }
                    return {};
                }
                case EBaseType::Uint64:
                case EBaseType::Int64:
                case EBaseType::Double:
                {
                    std::uint64_t value{};
                    if (!readLittleEndian(bytes, value))
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    if (base == EBaseType::Uint64)
                        *static_cast<std::uint64_t*>(destination) = value;
                    else if (base == EBaseType::Int64)
                        *static_cast<std::int64_t*>(destination) = static_cast<std::int64_t>(value);
                    else
                    {
                        const double number = std::bit_cast<double>(value);
                        if (!std::isfinite(number))
                            return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                        *static_cast<double*>(destination) = number;
                    }
                    return {};
                }
                case EBaseType::Record:
                    if (field.type.hash != lux::cxx::type_hash<std::string>())
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    *static_cast<std::string*>(destination) = std::string{
                        reinterpret_cast<const char*>(bytes.data()),
                        bytes.size(),
                    };
                    return {};
                default:
                    return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
            }
        }

        [[nodiscard]] lux::cxx::expected<void, EComponentCodecError>
        decodeProperty(
            const lux::meta::RefClass& reflection,
            void* object,
            std::string_view name,
            EComponentWireType type,
            std::span<const std::byte> bytes,
            const ComponentDecodePort& port
        ) noexcept
        {
            std::string_view remainder;
            const auto* field = findField(reflection, name, remainder);
            if (field == nullptr)
                return {};
            void* destination = fieldAddress(*field, object);
            if (remainder.empty())
                return decodeLeaf(*field, destination, type, bytes, port);
            if (static_cast<lux::meta::EBaseType>(field->type.qtype.base) !=
                    lux::meta::EBaseType::Record ||
                field->type.hash == lux::cxx::type_hash<std::string>() ||
                field->type.ptr == nullptr)
            {
                return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
            }
            return decodeProperty(
                *static_cast<const lux::meta::RefClass*>(field->type.ptr),
                destination,
                remainder,
                type,
                bytes,
                port
            );
        }

        lux::cxx::expected<void, EComponentCodecError> encodeReflected(
            const ComponentSchema& schema,
            const World& world,
            Entity entity,
            ComponentEncodePort& port
        ) noexcept
        {
            if (schema.reflection == nullptr || schema.operations.get_const == nullptr)
                return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
            const void* object = schema.operations.get_const(world, entity);
            if (object == nullptr)
                return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
            try
            {
                return encodeObject(*schema.reflection, object, {}, port);
            }
            catch (...)
            {
                return lux::cxx::unexpected(EComponentCodecError::ALLOCATION_FAILURE);
            }
        }

        lux::cxx::expected<void, EComponentCodecError> decodeReflected(
            const ComponentSchema& schema,
            WorldEdit& edit,
            Entity entity,
            std::uint32_t version,
            ComponentDecodePort& port
        ) noexcept
        {
            if (version != schema.version)
                return lux::cxx::unexpected(EComponentCodecError::UNSUPPORTED_VERSION);
            if (schema.reflection == nullptr ||
                schema.operations.default_emplace == nullptr ||
                schema.operations.erase == nullptr)
            {
                return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
            }

            bool inserted{};
            try
            {
                void* object = schema.operations.default_emplace(edit, entity);
                inserted = true;
                std::vector<std::string_view> seen;
                seen.reserve(schema.reflection->fields.size());
                EncodedPropertyView property;
                while (port.next(property))
                {
                    if (std::find(seen.begin(), seen.end(), property.name) != seen.end())
                    {
                        schema.operations.erase(edit, entity);
                        return lux::cxx::unexpected(EComponentCodecError::INVALID_DATA);
                    }
                    seen.push_back(property.name);
                    auto decoded = decodeProperty(
                        *schema.reflection,
                        object,
                        property.name,
                        property.type,
                        property.bytes,
                        port
                    );
                    if (!decoded)
                    {
                        schema.operations.erase(edit, entity);
                        return decoded;
                    }
                }
                return {};
            }
            catch (...)
            {
                if (inserted)
                    schema.operations.erase(edit, entity);
                return lux::cxx::unexpected(EComponentCodecError::ALLOCATION_FAILURE);
            }
        }
    } // namespace

    ComponentCodec reflectedComponentCodec() noexcept
    {
        return ComponentCodec{&encodeReflected, &decodeReflected};
    }
} // namespace lux::ecs
