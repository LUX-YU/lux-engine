#include <lux/engine/ecs/serialization/TaggedPropertyArchive.hpp>

#include <lux/engine/meta/Meta.hpp>

#include <lux/cxx/compile_time/type_info.hpp>

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <uuid.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lux::ecs::serialization
{
    namespace
    {
        using lux::serialize::ArchiveReader;
        using lux::serialize::ArchiveWriter;
        using lux::serialize::NameTable;

        constexpr std::size_t kFieldHeaderSize =
            sizeof(std::uint32_t) + sizeof(std::uint8_t) + sizeof(std::uint32_t);

        [[nodiscard]] ComponentArchiveResult<void> fail(
            EComponentArchiveError error,
            std::size_t offset,
            std::string path,
            std::string detail)
        {
            return lux::cxx::unexpected(ComponentArchiveFailure{
                .error = error,
                .byte_offset = offset,
                .field_path = std::move(path),
                .detail = std::move(detail),
            });
        }

        [[nodiscard]] std::string childPath(
            std::string_view parent,
            std::string_view child)
        {
            if (parent.empty())
                return std::string{child};
            std::string result;
            result.reserve(parent.size() + child.size() + 1u);
            result.append(parent);
            result.push_back('.');
            result.append(child);
            return result;
        }

        [[nodiscard]] const void* fieldPtr(
            const lux::meta::RefField& field,
            const void* object) noexcept
        {
            return static_cast<const std::byte*>(object) + field.offset;
        }

        [[nodiscard]] void* fieldPtr(
            const lux::meta::RefField& field,
            void* object) noexcept
        {
            return static_cast<std::byte*>(object) + field.offset;
        }

        [[nodiscard]] bool isExcludedField(
            const lux::meta::RefField& field) noexcept
        {
            if (field.visibility != lux::meta::EVisibility::Public)
                return true;
            if (!field.annotation_str)
                return false;
            const std::string_view annotations{field.annotation_str};
            return annotations.find("luxref::property::skip") !=
                       std::string_view::npos ||
                field.annotations().has("cooked_relocation");
        }

        [[nodiscard]] EArchiveType classifyField(
            const lux::meta::RefField& field) noexcept
        {
            const auto base =
                static_cast<lux::meta::EBaseType>(field.type.qtype.base);
            switch (base)
            {
                case lux::meta::EBaseType::Bool:   return EArchiveType::Bool;
                case lux::meta::EBaseType::Int32:  return EArchiveType::Int32;
                case lux::meta::EBaseType::Uint32: return EArchiveType::UInt32;
                case lux::meta::EBaseType::Int64:  return EArchiveType::Int64;
                case lux::meta::EBaseType::Uint64: return EArchiveType::UInt64;
                case lux::meta::EBaseType::Float:  return EArchiveType::Float;
                case lux::meta::EBaseType::Double: return EArchiveType::Double;
                case lux::meta::EBaseType::Int8:
                case lux::meta::EBaseType::Int16:
                    return EArchiveType::Int32;
                case lux::meta::EBaseType::Uint8:
                case lux::meta::EBaseType::Uint16:
                    return EArchiveType::UInt32;
                default:
                    break;
            }

            if (base != lux::meta::EBaseType::Record)
                return EArchiveType::Skip;
            if (field.type.hash == lux::cxx::type_hash<std::string>())
                return EArchiveType::String;
            if (field.type.hash == lux::cxx::type_hash<std::string_view>())
                return EArchiveType::Skip;
            if (field.type.hash == lux::cxx::type_hash<Eigen::Vector2f>())
                return EArchiveType::Vec2f;
            if (field.type.hash == lux::cxx::type_hash<Eigen::Vector3f>())
                return EArchiveType::Vec3f;
            if (field.type.hash == lux::cxx::type_hash<Eigen::Vector4f>())
                return EArchiveType::Vec4f;
            if (field.type.hash == lux::cxx::type_hash<Eigen::Quaternionf>())
                return EArchiveType::Quatf;
            if (field.type.hash == lux::cxx::type_hash<Eigen::Matrix4f>())
                return EArchiveType::Mat4f;
            if (field.type.hash == lux::cxx::type_hash<std::array<float, 8>>())
                return EArchiveType::ArrayFloat8;
            if (field.type.hash == lux::cxx::type_hash<uuids::uuid>())
                return EArchiveType::Uuid;
            return field.type.ptr ? EArchiveType::Struct : EArchiveType::Skip;
        }

        [[nodiscard]] std::size_t fixedPayloadSize(EArchiveType type) noexcept
        {
            switch (type)
            {
                case EArchiveType::Bool:        return 1u;
                case EArchiveType::Int32:       return 4u;
                case EArchiveType::UInt32:      return 4u;
                case EArchiveType::Int64:       return 8u;
                case EArchiveType::UInt64:      return 8u;
                case EArchiveType::Float:       return 4u;
                case EArchiveType::Double:      return 8u;
                case EArchiveType::Vec2f:       return 8u;
                case EArchiveType::Vec3f:       return 12u;
                case EArchiveType::Vec4f:       return 16u;
                case EArchiveType::Quatf:       return 16u;
                case EArchiveType::Mat4f:       return 64u;
                case EArchiveType::ArrayFloat8: return 32u;
                case EArchiveType::Uuid:        return 16u;
                default:                        return 0u;
            }
        }

        template <class Derived>
        [[nodiscard]] bool allFinite(const Eigen::DenseBase<Derived>& value)
        {
            return value.allFinite();
        }

        [[nodiscard]] bool nameExists(
            const NameTable& names,
            std::string_view candidate) noexcept
        {
            for (std::uint32_t index = 1u; index < names.size(); ++index)
            {
                if (names.at(index) == candidate)
                    return true;
            }
            return false;
        }

        [[nodiscard]] ComponentArchiveResult<void> validateNameTable(
            const NameTable& names,
            const ComponentArchiveLimits& limits)
        {
            if (names.size() == 0u || names.size() > limits.max_names)
                return fail(
                    EComponentArchiveError::LIMIT_EXCEEDED,
                    0u,
                    {},
                    "NameTable count is zero or exceeds the configured limit");
            if (!names.at(0u).empty())
                return fail(
                    EComponentArchiveError::INVALID_NAME_INDEX,
                    0u,
                    {},
                    "NameTable index zero is not the empty sentinel");
            for (std::uint32_t index = 1u; index < names.size(); ++index)
            {
                const auto name = names.at(index);
                if (name.empty() || name.size() > limits.max_name_bytes)
                    return fail(
                        EComponentArchiveError::LIMIT_EXCEEDED,
                        0u,
                        {},
                        "NameTable entry is empty or exceeds the name byte limit");
            }
            return {};
        }

        struct PreflightState
        {
            const ComponentArchiveLimits* limits{};
            const NameTable*               names{};
            std::unordered_set<std::string> new_names;
        };

        [[nodiscard]] ComponentArchiveResult<std::size_t> preflightObject(
            const lux::meta::RefClass& reflected_class,
            const void* object,
            std::uint32_t depth,
            std::string_view path,
            PreflightState& state)
        {
            if (!object)
                return lux::cxx::unexpected(ComponentArchiveFailure{
                    .error = EComponentArchiveError::INVALID_ARGUMENT,
                    .field_path = std::string{path},
                    .detail = "object pointer is null",
                });
            if (depth > state.limits->max_nesting_depth)
                return lux::cxx::unexpected(ComponentArchiveFailure{
                    .error = EComponentArchiveError::LIMIT_EXCEEDED,
                    .field_path = std::string{path},
                    .detail = "component archive nesting limit exceeded",
                });

            std::size_t encoded_size = sizeof(std::uint32_t);
            if (encoded_size > state.limits->max_object_bytes)
                return lux::cxx::unexpected(ComponentArchiveFailure{
                    .error = EComponentArchiveError::LIMIT_EXCEEDED,
                    .field_path = std::string{path},
                    .detail = "object byte limit is smaller than the end marker",
                });
            std::uint32_t field_count = 0u;
            for (const auto& field : reflected_class.fields)
            {
                if (isExcludedField(field))
                    continue;
                ++field_count;
                const auto field_path = childPath(path, field.name);
                if (field_count > state.limits->max_fields_per_object)
                    return lux::cxx::unexpected(ComponentArchiveFailure{
                        .error = EComponentArchiveError::LIMIT_EXCEEDED,
                        .field_path = field_path,
                        .detail = "field count limit exceeded",
                    });
                if (field.name.empty() ||
                    field.name.size() > state.limits->max_name_bytes)
                    return lux::cxx::unexpected(ComponentArchiveFailure{
                        .error = EComponentArchiveError::INVALID_REFLECTION,
                        .field_path = field_path,
                        .detail = "field name is empty or exceeds name limit",
                    });

                const auto type = classifyField(field);
                if (type == EArchiveType::Skip)
                    return lux::cxx::unexpected(ComponentArchiveFailure{
                        .error = EComponentArchiveError::UNSUPPORTED_FIELD_TYPE,
                        .field_path = field_path,
                        .detail = "field has no supported owning archive type",
                    });
                if (!nameExists(*state.names, field.name))
                    state.new_names.emplace(field.name);
                if (state.names->size() + state.new_names.size() >
                    state.limits->max_names)
                    return lux::cxx::unexpected(ComponentArchiveFailure{
                        .error = EComponentArchiveError::LIMIT_EXCEEDED,
                        .field_path = field_path,
                        .detail = "name table limit exceeded",
                    });

                const void* value = fieldPtr(field, object);
                std::size_t payload_size = fixedPayloadSize(type);
                switch (type)
                {
                    case EArchiveType::Float:
                        if (!std::isfinite(*static_cast<const float*>(value)))
                            return lux::cxx::unexpected(ComponentArchiveFailure{
                                .error = EComponentArchiveError::INVALID_VALUE,
                                .field_path = field_path,
                                .detail = "non-finite float",
                            });
                        break;
                    case EArchiveType::Double:
                        if (!std::isfinite(*static_cast<const double*>(value)))
                            return lux::cxx::unexpected(ComponentArchiveFailure{
                                .error = EComponentArchiveError::INVALID_VALUE,
                                .field_path = field_path,
                                .detail = "non-finite double",
                            });
                        break;
                    case EArchiveType::String:
                    {
                        const auto& text = *static_cast<const std::string*>(value);
                        if (text.size() > state.limits->max_string_bytes ||
                            text.size() > std::numeric_limits<std::uint32_t>::max())
                            return lux::cxx::unexpected(ComponentArchiveFailure{
                                .error = EComponentArchiveError::LIMIT_EXCEEDED,
                                .field_path = field_path,
                                .detail = "string length limit exceeded",
                            });
                        payload_size = sizeof(std::uint32_t) + text.size();
                        break;
                    }
                    case EArchiveType::Vec2f:
                        if (!allFinite(*static_cast<const Eigen::Vector2f*>(value)))
                            return lux::cxx::unexpected(ComponentArchiveFailure{
                                .error = EComponentArchiveError::INVALID_VALUE,
                                .field_path = field_path,
                                .detail = "non-finite Vec2f",
                            });
                        break;
                    case EArchiveType::Vec3f:
                        if (!allFinite(*static_cast<const Eigen::Vector3f*>(value)))
                            return lux::cxx::unexpected(ComponentArchiveFailure{
                                .error = EComponentArchiveError::INVALID_VALUE,
                                .field_path = field_path,
                                .detail = "non-finite Vec3f",
                            });
                        break;
                    case EArchiveType::Vec4f:
                        if (!allFinite(*static_cast<const Eigen::Vector4f*>(value)))
                            return lux::cxx::unexpected(ComponentArchiveFailure{
                                .error = EComponentArchiveError::INVALID_VALUE,
                                .field_path = field_path,
                                .detail = "non-finite Vec4f",
                            });
                        break;
                    case EArchiveType::Quatf:
                        if (!static_cast<const Eigen::Quaternionf*>(value)->coeffs().allFinite())
                            return lux::cxx::unexpected(ComponentArchiveFailure{
                                .error = EComponentArchiveError::INVALID_VALUE,
                                .field_path = field_path,
                                .detail = "non-finite Quatf",
                            });
                        break;
                    case EArchiveType::Mat4f:
                        if (!allFinite(*static_cast<const Eigen::Matrix4f*>(value)))
                            return lux::cxx::unexpected(ComponentArchiveFailure{
                                .error = EComponentArchiveError::INVALID_VALUE,
                                .field_path = field_path,
                                .detail = "non-finite Mat4f",
                            });
                        break;
                    case EArchiveType::ArrayFloat8:
                        for (const auto element :
                             *static_cast<const std::array<float, 8>*>(value))
                        {
                            if (!std::isfinite(element))
                                return lux::cxx::unexpected(ComponentArchiveFailure{
                                    .error = EComponentArchiveError::INVALID_VALUE,
                                    .field_path = field_path,
                                    .detail = "non-finite float array element",
                                });
                        }
                        break;
                    case EArchiveType::Struct:
                    {
                        const auto* nested =
                            static_cast<const lux::meta::RefClass*>(field.type.ptr);
                        if (!nested)
                            return lux::cxx::unexpected(ComponentArchiveFailure{
                                .error = EComponentArchiveError::INVALID_REFLECTION,
                                .field_path = field_path,
                                .detail = "Struct field has no RefClass",
                            });
                        auto nested_size = preflightObject(
                            *nested, value, depth + 1u, field_path, state);
                        if (!nested_size)
                            return lux::cxx::unexpected(std::move(nested_size.error()));
                        payload_size = *nested_size;
                        break;
                    }
                    default:
                        break;
                }

                if (payload_size > std::numeric_limits<std::uint32_t>::max() ||
                    kFieldHeaderSize > state.limits->max_object_bytes ||
                    payload_size >
                        state.limits->max_object_bytes - kFieldHeaderSize ||
                    encoded_size > state.limits->max_object_bytes -
                        (kFieldHeaderSize + payload_size))
                    return lux::cxx::unexpected(ComponentArchiveFailure{
                        .error = EComponentArchiveError::LIMIT_EXCEEDED,
                        .field_path = field_path,
                        .detail = "object byte limit exceeded",
                    });
                encoded_size += kFieldHeaderSize + payload_size;
            }
            return encoded_size;
        }

        void writeLeafPayload(
            EArchiveType type,
            const lux::meta::RefField& field,
            const void* value,
            ArchiveWriter& archive)
        {
            const auto base =
                static_cast<lux::meta::EBaseType>(field.type.qtype.base);
            switch (type)
            {
                case EArchiveType::Bool:
                    archive.writePod<std::uint8_t>(
                        *static_cast<const bool*>(value) ? 1u : 0u);
                    break;
                case EArchiveType::Int32:
                {
                    std::int32_t widened{};
                    if (base == lux::meta::EBaseType::Int8)
                        widened = *static_cast<const std::int8_t*>(value);
                    else if (base == lux::meta::EBaseType::Int16)
                        widened = *static_cast<const std::int16_t*>(value);
                    else
                        widened = *static_cast<const std::int32_t*>(value);
                    archive.writePod(widened);
                    break;
                }
                case EArchiveType::UInt32:
                {
                    std::uint32_t widened{};
                    if (base == lux::meta::EBaseType::Uint8)
                        widened = *static_cast<const std::uint8_t*>(value);
                    else if (base == lux::meta::EBaseType::Uint16)
                        widened = *static_cast<const std::uint16_t*>(value);
                    else
                        widened = *static_cast<const std::uint32_t*>(value);
                    archive.writePod(widened);
                    break;
                }
                case EArchiveType::Int64:
                    archive.writePod(*static_cast<const std::int64_t*>(value));
                    break;
                case EArchiveType::UInt64:
                    archive.writePod(*static_cast<const std::uint64_t*>(value));
                    break;
                case EArchiveType::Float:
                    archive.writePod(*static_cast<const float*>(value));
                    break;
                case EArchiveType::Double:
                    archive.writePod(*static_cast<const double*>(value));
                    break;
                case EArchiveType::String:
                    archive.writeString(*static_cast<const std::string*>(value));
                    break;
                case EArchiveType::Vec2f:
                    archive.writeBytes(
                        static_cast<const Eigen::Vector2f*>(value)->data(),
                        2u * sizeof(float));
                    break;
                case EArchiveType::Vec3f:
                    archive.writeBytes(
                        static_cast<const Eigen::Vector3f*>(value)->data(),
                        3u * sizeof(float));
                    break;
                case EArchiveType::Vec4f:
                    archive.writeBytes(
                        static_cast<const Eigen::Vector4f*>(value)->data(),
                        4u * sizeof(float));
                    break;
                case EArchiveType::Quatf:
                {
                    const auto& quaternion =
                        *static_cast<const Eigen::Quaternionf*>(value);
                    const float xyzw[4]{
                        quaternion.x(), quaternion.y(),
                        quaternion.z(), quaternion.w()};
                    archive.writeBytes(xyzw, sizeof(xyzw));
                    break;
                }
                case EArchiveType::Mat4f:
                    archive.writeBytes(
                        static_cast<const Eigen::Matrix4f*>(value)->data(),
                        16u * sizeof(float));
                    break;
                case EArchiveType::ArrayFloat8:
                    archive.writeBytes(
                        static_cast<const std::array<float, 8>*>(value)->data(),
                        8u * sizeof(float));
                    break;
                case EArchiveType::Uuid:
                    archive.writeUuid(*static_cast<const uuids::uuid*>(value));
                    break;
                default:
                    break;
            }
        }

        void writeObjectUnchecked(
            const lux::meta::RefClass& reflected_class,
            const void* object,
            ArchiveWriter& archive,
            NameTable& names)
        {
            for (const auto& field : reflected_class.fields)
            {
                if (isExcludedField(field))
                    continue;
                const auto type = classifyField(field);
                archive.writePod(names.intern(field.name));
                archive.writePod(static_cast<std::uint8_t>(type));
                const auto size_offset = archive.reserveU32();
                const auto payload_begin = archive.tell();
                if (type == EArchiveType::Struct)
                {
                    writeObjectUnchecked(
                        *static_cast<const lux::meta::RefClass*>(field.type.ptr),
                        fieldPtr(field, object),
                        archive,
                        names);
                }
                else
                {
                    writeLeafPayload(
                        type, field, fieldPtr(field, object), archive);
                }
                archive.patchU32At(
                    size_offset,
                    static_cast<std::uint32_t>(archive.tell() - payload_begin));
            }
            archive.writePod(kEndOfObject);
        }

        [[nodiscard]] ComponentArchiveResult<void> validateObjectSpan(
            std::span<const std::byte> payload,
            std::uint32_t name_count,
            const ComponentArchiveLimits& limits,
            std::uint32_t depth,
            std::size_t base_offset,
            std::string_view path)
        {
            if (depth > limits.max_nesting_depth)
                return fail(
                    EComponentArchiveError::LIMIT_EXCEEDED,
                    base_offset,
                    std::string{path},
                    "component archive nesting limit exceeded");
            if (payload.size() > limits.max_object_bytes)
                return fail(
                    EComponentArchiveError::LIMIT_EXCEEDED,
                    base_offset,
                    std::string{path},
                    "object byte limit exceeded");

            ArchiveReader reader{payload.data(), payload.size()};
            std::uint32_t field_count{};
            while (true)
            {
                const auto field_offset = base_offset + reader.tell();
                const auto name_index = reader.readPod<std::uint32_t>();
                if (!reader.ok())
                    return fail(
                        EComponentArchiveError::TRUNCATED,
                        field_offset,
                        std::string{path},
                        "truncated field name index");
                if (name_index == kEndOfObject)
                {
                    if (!reader.eof())
                        return fail(
                            EComponentArchiveError::TRAILING_BYTES,
                            base_offset + reader.tell(),
                            std::string{path},
                            "bytes follow end-of-object marker");
                    return {};
                }
                if (name_index == 0u || name_index >= name_count)
                    return fail(
                        EComponentArchiveError::INVALID_NAME_INDEX,
                        field_offset,
                        std::string{path},
                        "field name index is outside the NameTable");
                if (++field_count > limits.max_fields_per_object)
                    return fail(
                        EComponentArchiveError::LIMIT_EXCEEDED,
                        field_offset,
                        std::string{path},
                        "field count limit exceeded");

                const auto type = static_cast<EArchiveType>(
                    reader.readPod<std::uint8_t>());
                const auto payload_size = reader.readPod<std::uint32_t>();
                if (!reader.ok())
                    return fail(
                        EComponentArchiveError::TRUNCATED,
                        field_offset,
                        std::string{path},
                        "truncated field header");
                const auto field_payload_offset = base_offset + reader.tell();
                const auto field_payload = reader.readSpan(payload_size);
                if (!reader.ok())
                    return fail(
                        EComponentArchiveError::TRUNCATED,
                        field_payload_offset,
                        std::string{path},
                        "declared field payload exceeds object boundary");

                if (type == EArchiveType::Struct)
                {
                    auto nested = validateObjectSpan(
                        field_payload,
                        name_count,
                        limits,
                        depth + 1u,
                        field_payload_offset,
                        path);
                    if (!nested)
                        return nested;
                }
                else if (type == EArchiveType::String)
                {
                    ArchiveReader text_reader{
                        field_payload.data(), field_payload.size()};
                    const auto length = text_reader.readPod<std::uint32_t>();
                    if (!text_reader.ok() || length > text_reader.remaining())
                        return fail(
                            EComponentArchiveError::TRUNCATED,
                            field_payload_offset,
                            std::string{path},
                            "truncated string payload");
                    if (length > limits.max_string_bytes)
                        return fail(
                            EComponentArchiveError::LIMIT_EXCEEDED,
                            field_payload_offset,
                            std::string{path},
                            "string length limit exceeded");
                }
                else if (const auto fixed = fixedPayloadSize(type); fixed != 0u &&
                         payload_size < fixed)
                {
                    return fail(
                        EComponentArchiveError::PAYLOAD_SIZE_MISMATCH,
                        field_payload_offset,
                        std::string{path},
                        "known leaf payload is smaller than its wire width");
                }
                else if (type == EArchiveType::Bool &&
                         std::to_integer<std::uint8_t>(field_payload[0]) > 1u)
                {
                    return fail(
                        EComponentArchiveError::INVALID_VALUE,
                        field_payload_offset,
                        std::string{path},
                        "boolean payload is not 0 or 1");
                }
                else if (type == EArchiveType::Float ||
                         type == EArchiveType::Vec2f ||
                         type == EArchiveType::Vec3f ||
                         type == EArchiveType::Vec4f ||
                         type == EArchiveType::Quatf ||
                         type == EArchiveType::Mat4f ||
                         type == EArchiveType::ArrayFloat8)
                {
                    ArchiveReader value_reader{
                        field_payload.data(), field_payload.size()};
                    const auto count = fixedPayloadSize(type) / sizeof(float);
                    for (std::size_t index = 0u; index < count; ++index)
                    {
                        if (!std::isfinite(value_reader.readPod<float>()))
                            return fail(
                                EComponentArchiveError::INVALID_VALUE,
                                field_payload_offset + index * sizeof(float),
                                std::string{path},
                                "known float payload contains a non-finite value");
                    }
                }
                else if (type == EArchiveType::Double)
                {
                    ArchiveReader value_reader{
                        field_payload.data(), field_payload.size()};
                    if (!std::isfinite(value_reader.readPod<double>()))
                        return fail(
                            EComponentArchiveError::INVALID_VALUE,
                            field_payload_offset,
                            std::string{path},
                            "double payload is non-finite");
                }
            }
        }

        [[nodiscard]] const lux::meta::RefField* findField(
            const lux::meta::RefClass& reflected_class,
            std::string_view name) noexcept
        {
            for (const auto& field : reflected_class.fields)
            {
                if (!isExcludedField(field) && field.name == name)
                    return &field;
            }
            return nullptr;
        }

        [[nodiscard]] std::vector<const lux::meta::RefField*> serializedFields(
            const lux::meta::RefClass& reflected_class)
        {
            std::vector<const lux::meta::RefField*> result;
            result.reserve(reflected_class.fields.size());
            for (const auto& field : reflected_class.fields)
            {
                if (!isExcludedField(field))
                    result.push_back(&field);
            }
            return result;
        }

        [[nodiscard]] ComponentArchiveResult<void> readLeaf(
            EArchiveType type,
            const lux::meta::RefField& field,
            void* destination,
            std::span<const std::byte> payload,
            bool exact,
            const ComponentArchiveLimits& limits,
            std::size_t offset,
            std::string path)
        {
            const auto required = fixedPayloadSize(type);
            if ((required && payload.size() < required) ||
                (exact && required && payload.size() != required))
                return fail(
                    EComponentArchiveError::PAYLOAD_SIZE_MISMATCH,
                    offset,
                    std::move(path),
                    "leaf payload width does not match field type");

            ArchiveReader reader{payload.data(), payload.size()};
            const auto base =
                static_cast<lux::meta::EBaseType>(field.type.qtype.base);
            switch (type)
            {
                case EArchiveType::Bool:
                {
                    const auto value = reader.readPod<std::uint8_t>();
                    if (value > 1u)
                        return fail(
                            EComponentArchiveError::INVALID_VALUE,
                            offset,
                            std::move(path),
                            "boolean payload is not 0 or 1");
                    *static_cast<bool*>(destination) = value != 0u;
                    break;
                }
                case EArchiveType::Int32:
                {
                    const auto value = reader.readPod<std::int32_t>();
                    if (base == lux::meta::EBaseType::Int8)
                    {
                        if (value < std::numeric_limits<std::int8_t>::min() ||
                            value > std::numeric_limits<std::int8_t>::max())
                            return fail(
                                EComponentArchiveError::INVALID_VALUE,
                                offset,
                                std::move(path),
                                "Int32 payload does not fit int8 field");
                        *static_cast<std::int8_t*>(destination) =
                            static_cast<std::int8_t>(value);
                    }
                    else if (base == lux::meta::EBaseType::Int16)
                    {
                        if (value < std::numeric_limits<std::int16_t>::min() ||
                            value > std::numeric_limits<std::int16_t>::max())
                            return fail(
                                EComponentArchiveError::INVALID_VALUE,
                                offset,
                                std::move(path),
                                "Int32 payload does not fit int16 field");
                        *static_cast<std::int16_t*>(destination) =
                            static_cast<std::int16_t>(value);
                    }
                    else
                    {
                        *static_cast<std::int32_t*>(destination) = value;
                    }
                    break;
                }
                case EArchiveType::UInt32:
                {
                    const auto value = reader.readPod<std::uint32_t>();
                    if (base == lux::meta::EBaseType::Uint8)
                    {
                        if (value > std::numeric_limits<std::uint8_t>::max())
                            return fail(
                                EComponentArchiveError::INVALID_VALUE,
                                offset,
                                std::move(path),
                                "UInt32 payload does not fit uint8 field");
                        *static_cast<std::uint8_t*>(destination) =
                            static_cast<std::uint8_t>(value);
                    }
                    else if (base == lux::meta::EBaseType::Uint16)
                    {
                        if (value > std::numeric_limits<std::uint16_t>::max())
                            return fail(
                                EComponentArchiveError::INVALID_VALUE,
                                offset,
                                std::move(path),
                                "UInt32 payload does not fit uint16 field");
                        *static_cast<std::uint16_t*>(destination) =
                            static_cast<std::uint16_t>(value);
                    }
                    else
                    {
                        *static_cast<std::uint32_t*>(destination) = value;
                    }
                    break;
                }
                case EArchiveType::Int64:
                    *static_cast<std::int64_t*>(destination) =
                        reader.readPod<std::int64_t>();
                    break;
                case EArchiveType::UInt64:
                    *static_cast<std::uint64_t*>(destination) =
                        reader.readPod<std::uint64_t>();
                    break;
                case EArchiveType::Float:
                {
                    const auto value = reader.readPod<float>();
                    if (!std::isfinite(value))
                        return fail(
                            EComponentArchiveError::INVALID_VALUE,
                            offset,
                            std::move(path),
                            "non-finite float");
                    *static_cast<float*>(destination) = value;
                    break;
                }
                case EArchiveType::Double:
                {
                    const auto value = reader.readPod<double>();
                    if (!std::isfinite(value))
                        return fail(
                            EComponentArchiveError::INVALID_VALUE,
                            offset,
                            std::move(path),
                            "non-finite double");
                    *static_cast<double*>(destination) = value;
                    break;
                }
                case EArchiveType::String:
                {
                    const auto length = reader.readPod<std::uint32_t>();
                    if (!reader.ok() || length > reader.remaining())
                        return fail(
                            EComponentArchiveError::TRUNCATED,
                            offset,
                            std::move(path),
                            "truncated string payload");
                    if (length > limits.max_string_bytes)
                        return fail(
                            EComponentArchiveError::LIMIT_EXCEEDED,
                            offset,
                            std::move(path),
                            "string length limit exceeded");
                    const auto bytes = reader.readSpan(length);
                    *static_cast<std::string*>(destination) = std::string{
                        reinterpret_cast<const char*>(bytes.data()), bytes.size()};
                    break;
                }
                case EArchiveType::Vec2f:
                    reader.readBytes(destination, 2u * sizeof(float));
                    if (!static_cast<Eigen::Vector2f*>(destination)->allFinite())
                        return fail(EComponentArchiveError::INVALID_VALUE, offset,
                                    std::move(path), "non-finite Vec2f");
                    break;
                case EArchiveType::Vec3f:
                    reader.readBytes(destination, 3u * sizeof(float));
                    if (!static_cast<Eigen::Vector3f*>(destination)->allFinite())
                        return fail(EComponentArchiveError::INVALID_VALUE, offset,
                                    std::move(path), "non-finite Vec3f");
                    break;
                case EArchiveType::Vec4f:
                    reader.readBytes(destination, 4u * sizeof(float));
                    if (!static_cast<Eigen::Vector4f*>(destination)->allFinite())
                        return fail(EComponentArchiveError::INVALID_VALUE, offset,
                                    std::move(path), "non-finite Vec4f");
                    break;
                case EArchiveType::Quatf:
                {
                    float xyzw[4]{};
                    reader.readBytes(xyzw, sizeof(xyzw));
                    if (!std::all_of(
                            std::begin(xyzw), std::end(xyzw),
                            [](float value) { return std::isfinite(value); }))
                        return fail(EComponentArchiveError::INVALID_VALUE, offset,
                                    std::move(path), "non-finite Quatf");
                    auto& value = *static_cast<Eigen::Quaternionf*>(destination);
                    value.x() = xyzw[0];
                    value.y() = xyzw[1];
                    value.z() = xyzw[2];
                    value.w() = xyzw[3];
                    break;
                }
                case EArchiveType::Mat4f:
                    reader.readBytes(destination, 16u * sizeof(float));
                    if (!static_cast<Eigen::Matrix4f*>(destination)->allFinite())
                        return fail(EComponentArchiveError::INVALID_VALUE, offset,
                                    std::move(path), "non-finite Mat4f");
                    break;
                case EArchiveType::ArrayFloat8:
                {
                    auto& value = *static_cast<std::array<float, 8>*>(destination);
                    reader.readBytes(value.data(), 8u * sizeof(float));
                    if (!std::all_of(
                            value.begin(), value.end(),
                            [](float element) { return std::isfinite(element); }))
                        return fail(EComponentArchiveError::INVALID_VALUE, offset,
                                    std::move(path), "non-finite float array");
                    break;
                }
                case EArchiveType::Uuid:
                    *static_cast<uuids::uuid*>(destination) = reader.readUuid();
                    break;
                default:
                    return fail(
                        EComponentArchiveError::UNSUPPORTED_FIELD_TYPE,
                        offset,
                        std::move(path),
                        "field is not a supported leaf type");
            }

            if (!reader.ok())
                return fail(
                    EComponentArchiveError::TRUNCATED,
                    offset + reader.tell(),
                    std::move(path),
                    "truncated leaf payload");
            if (exact && !reader.eof())
                return fail(
                    EComponentArchiveError::PAYLOAD_SIZE_MISMATCH,
                    offset + reader.tell(),
                    std::move(path),
                    "bytes follow exact leaf payload");
            return {};
        }

        [[nodiscard]] ComponentArchiveResult<void> decodeObject(
            std::span<const std::byte> payload,
            const NameTable& names,
            const lux::meta::RefClass& reflected_class,
            void* object,
            bool exact,
            const ComponentArchiveLimits& limits,
            std::uint32_t depth,
            std::size_t base_offset,
            std::string_view path)
        {
            auto framed = validateObjectSpan(
                payload, names.size(), limits, depth, base_offset, path);
            if (!framed)
                return framed;

            const auto expected_fields = serializedFields(reflected_class);
            std::size_t exact_index{};
            std::unordered_set<const lux::meta::RefField*> seen;
            ArchiveReader reader{payload.data(), payload.size()};
            while (true)
            {
                const auto record_offset = base_offset + reader.tell();
                const auto name_index = reader.readPod<std::uint32_t>();
                if (name_index == kEndOfObject)
                    break;
                const auto wire_type = static_cast<EArchiveType>(
                    reader.readPod<std::uint8_t>());
                const auto payload_size = reader.readPod<std::uint32_t>();
                const auto field_offset = base_offset + reader.tell();
                const auto field_payload = reader.readSpan(payload_size);
                const auto name = names.at(name_index);
                const auto* field = findField(reflected_class, name);

                if (exact)
                {
                    if (exact_index >= expected_fields.size() ||
                        expected_fields[exact_index] != field)
                        return fail(
                            EComponentArchiveError::NON_CANONICAL_OBJECT,
                            record_offset,
                            childPath(path, name),
                            "field is missing, duplicated, unknown or reordered");
                    ++exact_index;
                }
                else if (field && !seen.emplace(field).second)
                {
                    return fail(
                        EComponentArchiveError::NON_CANONICAL_OBJECT,
                        record_offset,
                        childPath(path, name),
                        "known field appears more than once");
                }

                if (!field)
                    continue;
                const auto expected_type = classifyField(*field);
                const auto field_path = childPath(path, field->name);
                if (expected_type == EArchiveType::Skip)
                    return fail(
                        EComponentArchiveError::UNSUPPORTED_FIELD_TYPE,
                        record_offset,
                        field_path,
                        "known field has no supported archive type");
                if (wire_type != expected_type)
                {
                    if (exact)
                        return fail(
                            EComponentArchiveError::TYPE_MISMATCH,
                            record_offset,
                            field_path,
                            "wire type does not match reflected field type");
                    continue;
                }

                if (wire_type == EArchiveType::Struct)
                {
                    const auto* nested =
                        static_cast<const lux::meta::RefClass*>(field->type.ptr);
                    if (!nested)
                        return fail(
                            EComponentArchiveError::INVALID_REFLECTION,
                            record_offset,
                            field_path,
                            "Struct field has no RefClass");
                    auto decoded = decodeObject(
                        field_payload,
                        names,
                        *nested,
                        fieldPtr(*field, object),
                        exact,
                        limits,
                        depth + 1u,
                        field_offset,
                        field_path);
                    if (!decoded)
                        return decoded;
                }
                else
                {
                    auto decoded = readLeaf(
                        wire_type,
                        *field,
                        fieldPtr(*field, object),
                        field_payload,
                        exact,
                        limits,
                        field_offset,
                        field_path);
                    if (!decoded)
                        return decoded;
                }
            }

            if (exact && exact_index != expected_fields.size())
            {
                return fail(
                    EComponentArchiveError::NON_CANONICAL_OBJECT,
                    base_offset + reader.tell(),
                    std::string{path},
                    "exact object is missing reflected fields");
            }
            return {};
        }
    }

    ComponentArchiveResult<void> TaggedPropertyWriter::writeObject(
        const lux::meta::RefClass& reflected_class,
        const void* object)
    {
        auto valid_names = validateNameTable(*names_, limits_);
        if (!valid_names)
            return valid_names;
        PreflightState state{
            .limits = &limits_,
            .names = names_,
        };
        auto checked = preflightObject(
            reflected_class, object, 0u, {}, state);
        if (!checked)
            return lux::cxx::unexpected(std::move(checked.error()));
        writeObjectUnchecked(reflected_class, object, *archive_, *names_);
        return {};
    }

    ComponentArchiveResult<void> TaggedPropertyReader::readObject(
        const lux::meta::RefClass& reflected_class,
        void* object)
    {
        if (!object || !archive_->ok())
        {
            archive_->invalidate();
            return fail(
                EComponentArchiveError::INVALID_ARGUMENT,
                archive_->tell(),
                {},
                "reader is invalid or destination is null");
        }
        auto valid_names = validateNameTable(*names_, limits_);
        if (!valid_names)
        {
            archive_->invalidate();
            return valid_names;
        }
        const auto payload = archive_->remainingSpan();
        auto result = decodeObject(
            payload,
            *names_,
            reflected_class,
            object,
            false,
            limits_,
            0u,
            0u,
            {});
        if (!result)
        {
            archive_->invalidate();
            return result;
        }
        archive_->skip(payload.size());
        return {};
    }

    ComponentArchiveResult<void> TaggedPropertyReader::readObjectExact(
        const lux::meta::RefClass& reflected_class,
        void* object)
    {
        if (!object || !archive_->ok())
        {
            archive_->invalidate();
            return fail(
                EComponentArchiveError::INVALID_ARGUMENT,
                archive_->tell(),
                {},
                "reader is invalid or destination is null");
        }
        auto valid_names = validateNameTable(*names_, limits_);
        if (!valid_names)
        {
            archive_->invalidate();
            return valid_names;
        }
        const auto payload = archive_->remainingSpan();
        auto result = decodeObject(
            payload,
            *names_,
            reflected_class,
            object,
            true,
            limits_,
            0u,
            0u,
            {});
        if (!result)
        {
            archive_->invalidate();
            return result;
        }
        archive_->skip(payload.size());
        return {};
    }

    ComponentArchiveResult<void> validateTaggedPropertyObject(
        std::span<const std::byte> payload,
        std::uint32_t name_count,
        ComponentArchiveLimits limits)
    {
        if (name_count == 0u || name_count > limits.max_names)
            return fail(
                EComponentArchiveError::INVALID_NAME_INDEX,
                0u,
                {},
                "NameTable count is zero or exceeds the configured limit");
        return validateObjectSpan(payload, name_count, limits, 0u, 0u, {});
    }

    bool isAssetReferenceField(const lux::meta::RefField& field) noexcept
    {
        if (classifyField(field) != EArchiveType::Uuid)
            return false;
        const auto annotation = field.annotations().get("asset_type");
        return annotation && !annotation->empty();
    }
}
