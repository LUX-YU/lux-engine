#pragma once

#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/schema/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <array>
#include <span>
#include <string_view>

namespace lux::ecs
{
    struct ComponentSchema;

    enum class EComponentWireType : std::uint8_t
    {
        BYTES,
        UNSIGNED_INTEGER,
        SIGNED_INTEGER,
        FLOATING_POINT,
        UTF8,
        LOCAL_ENTITY,
        STABLE_REFERENCE,
    };

    enum class EComponentCodecError : std::uint8_t
    {
        INVALID_DATA,
        UNSUPPORTED_VERSION,
        UNKNOWN_REFERENCE,
        LIMIT_EXCEEDED,
        ALLOCATION_FAILURE,
    };

    struct EncodedPropertyView final
    {
        std::string_view name;
        EComponentWireType type{EComponentWireType::BYTES};
        std::span<const std::byte> bytes;
    };

    class ComponentEncodePort
    {
      public:
        virtual ~ComponentEncodePort() = default;

        // `bytes` is a codec-owned portable representation. `type` describes
        // that representation; it does not endian-convert arbitrary host
        // integer or floating-point object bytes for the codec.
        [[nodiscard]] virtual lux::cxx::expected<void, EComponentCodecError>
        write(
            std::string_view name,
            EComponentWireType type,
            std::span<const std::byte> bytes
        ) noexcept = 0;

        [[nodiscard]] virtual lux::cxx::expected<void, EComponentCodecError>
        writeEntity(std::string_view name, Entity entity) noexcept = 0;

        [[nodiscard]] virtual lux::cxx::expected<void, EComponentCodecError>
        writeStableReference(
            std::string_view name,
            std::span<const std::byte> stable_id
        ) noexcept = 0;
    };

    class ComponentDecodePort
    {
      public:
        virtual ~ComponentDecodePort() = default;

        [[nodiscard]] virtual bool next(EncodedPropertyView& property) noexcept = 0;

        [[nodiscard]] virtual lux::cxx::expected<Entity, EComponentCodecError>
        resolveEntity(std::span<const std::byte> encoded) const noexcept = 0;

        [[nodiscard]] virtual lux::cxx::expected<
            std::array<std::byte, 16>,
            EComponentCodecError>
        resolveStableReference(
            std::span<const std::byte> encoded
        ) const noexcept = 0;
    };

    using EncodeComponentFn = lux::cxx::expected<void, EComponentCodecError> (*)(
        const ComponentSchema&,
        const World&,
        Entity,
        ComponentEncodePort&) noexcept;

    using DecodeComponentFn = lux::cxx::expected<void, EComponentCodecError> (*)(
        const ComponentSchema&,
        WorldEdit&,
        Entity,
        std::uint32_t,
        ComponentDecodePort&) noexcept;

    struct ComponentCodec final
    {
        EncodeComponentFn encode{};
        DecodeComponentFn decode{};
        const void* context{};

        [[nodiscard]] bool present() const noexcept
        {
            return encode != nullptr && decode != nullptr;
        }
    };

} // namespace lux::ecs
