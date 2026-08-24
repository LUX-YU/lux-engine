#pragma once

#include <lux/engine/core/serialization/Archive.hpp>
#include <lux/engine/core/serialization/NameTable.hpp>
#include <lux/engine/ecs/serialization/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace lux::meta
{
    struct RefClass;
    struct RefField;
}

namespace lux::ecs::serialization
{
    /// Stable tagged-property wire ordinals. Values are frozen and must never
    /// be renumbered. Uuid describes only the 16-byte value representation;
    /// asset semantics are supplied by field metadata.
    enum class EArchiveType : std::uint8_t
    {
        Skip        = 0,
        Bool        = 1,
        Int32       = 2,
        UInt32      = 3,
        Int64       = 4,
        UInt64      = 5,
        Float       = 6,
        Double      = 7,
        String      = 16,
        Vec2f       = 17,
        Vec3f       = 18,
        Vec4f       = 19,
        Quatf       = 20,
        Mat4f       = 21,
        ArrayFloat8 = 32,
        Uuid        = 48,
        Struct      = 64,
    };

    inline constexpr std::uint32_t kEndOfObject = 0xFFFFFFFFu;

    struct ComponentArchiveLimits
    {
        std::size_t   max_object_bytes{1ull << 30u};
        std::size_t   max_string_bytes{16ull << 20u};
        std::uint32_t max_fields_per_object{65536u};
        std::uint32_t max_names{1u << 20u};
        std::size_t   max_name_bytes{4096u};
        std::uint32_t max_nesting_depth{64u};
    };

    enum class EComponentArchiveError : std::uint8_t
    {
        INVALID_ARGUMENT,
        UNSUPPORTED_FIELD_TYPE,
        INVALID_REFLECTION,
        LIMIT_EXCEEDED,
        TRUNCATED,
        INVALID_NAME_INDEX,
        TYPE_MISMATCH,
        PAYLOAD_SIZE_MISMATCH,
        INVALID_VALUE,
        NON_CANONICAL_OBJECT,
        TRAILING_BYTES,
    };

    struct ComponentArchiveFailure
    {
        EComponentArchiveError error{EComponentArchiveError::INVALID_ARGUMENT};
        std::size_t            byte_offset{};
        std::string            field_path;
        std::string            detail;
    };

    template <class T>
    using ComponentArchiveResult =
        lux::cxx::expected<T, ComponentArchiveFailure>;

    class LUX_ECS_COMPONENT_ARCHIVE_PUBLIC TaggedPropertyWriter
    {
    public:
        TaggedPropertyWriter(
            lux::serialize::ArchiveWriter& archive,
            lux::serialize::NameTable& names,
            ComponentArchiveLimits limits = {}) noexcept
            : archive_(&archive), names_(&names), limits_(limits)
        {
        }

        [[nodiscard]] ComponentArchiveResult<void> writeObject(
            const lux::meta::RefClass& reflected_class,
            const void* object);

    private:
        lux::serialize::ArchiveWriter* archive_;
        lux::serialize::NameTable*     names_;
        ComponentArchiveLimits         limits_;
    };

    class LUX_ECS_COMPONENT_ARCHIVE_PUBLIC TaggedPropertyReader
    {
    public:
        TaggedPropertyReader(
            lux::serialize::ArchiveReader& archive,
            const lux::serialize::NameTable& names,
            ComponentArchiveLimits limits = {}) noexcept
            : archive_(&archive), names_(&names), limits_(limits)
        {
        }

        [[nodiscard]] ComponentArchiveResult<void> readObject(
            const lux::meta::RefClass& reflected_class,
            void* object);

        [[nodiscard]] ComponentArchiveResult<void> readObjectExact(
            const lux::meta::RefClass& reflected_class,
            void* object);

    private:
        lux::serialize::ArchiveReader* archive_;
        const lux::serialize::NameTable* names_;
        ComponentArchiveLimits limits_;
    };

    [[nodiscard]] LUX_ECS_COMPONENT_ARCHIVE_PUBLIC ComponentArchiveResult<void>
    validateTaggedPropertyObject(
        std::span<const std::byte> payload,
        std::uint32_t name_count,
        ComponentArchiveLimits limits = {});

    [[nodiscard]] LUX_ECS_COMPONENT_ARCHIVE_PUBLIC bool isAssetReferenceField(
        const lux::meta::RefField& field) noexcept;
}
