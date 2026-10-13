#pragma once

#include <concepts>
#include <memory>
#include <utility>
#include <vector>
#include <lux/cxx/binary/BinaryReader.hpp>
#include <lux/cxx/binary/BinaryWriter.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/render/data/Schema.hpp>

namespace lux::render
{
    using OwnedWireBytes = cxx::SharedBytes<>;

    struct BlobRef final
    {
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };

    struct ExternalDataRef final
    {
        std::uint32_t attachment_index = 0;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };

    struct WireLimits final
    {
        std::size_t max_wire_bytes = 0;
        std::size_t max_reference_bytes = 0;
    };

    // Synchronous borrow only. Resolve a reference to retain its actual byte owner.
    // Source owners must keep their backing storage stable and immutable.
    struct WireView final
    {
        const DataSchemaDescriptor& schema;
        const OwnedWireBytes& bytes;
        std::span<const OwnedWireBytes> attachments;
    };

    [[nodiscard]] RenderResult<OwnedWireBytes>
    resolveBlob(const OwnedWireBytes& owner, BlobRef ref, std::size_t limit, std::uint32_t alignment = 1) noexcept;

    [[nodiscard]] RenderResult<OwnedWireBytes> resolveExternal(
        std::span<const OwnedWireBytes> owners,
        ExternalDataRef ref,
        std::size_t limit,
        std::uint32_t alignment = 1
    ) noexcept;

    [[nodiscard]] RenderResult<void> validateWire(WireView wire, WireLimits limits) noexcept;

    // Explicit field codecs are required. Trivial copyability alone is not an
    // ownership proof, and there is deliberately no memcpy fallback for any T.
    template <typename T>
    concept WireData = requires(const T& value, cxx::BinarySpanWriter& writer, cxx::BinaryReader& reader, WireView wire)
    {
        { DataSchema<T>::describe() } noexcept -> std::same_as<const DataSchemaDescriptor&>;
        { DataSchema<T>::wireSize(value) } noexcept -> std::same_as<RenderResult<std::size_t>>;
        { DataSchema<T>::encode(value, writer) } noexcept -> std::same_as<RenderResult<void>>;
        { DataSchema<T>::decode(reader, wire) } noexcept -> std::same_as<RenderResult<T>>;
    };

    template <WireData T>
    [[nodiscard]] RenderResult<OwnedWireBytes>
    encodeWire(const T& value, std::span<const OwnedWireBytes> attachments, WireLimits limits) noexcept
    {
        const auto& schema = DataSchema<T>::describe();
        if (auto result = validateDataSchema(schema); !result) return cxx::unexpected(result.error());
        auto size = DataSchema<T>::wireSize(value);
        if (!size) return cxx::unexpected(size.error());
        if (*size > limits.max_wire_bytes) return cxx::unexpected(RenderError{kWireLimit, {*size}});
        if (*size < schema.fixed_size) return cxx::unexpected(RenderError{kInvalidWire, {*size}});

        // One retained backing allocation; no copy when publishing owned bytes.
        auto storage = std::make_shared<std::vector<std::byte>>(*size);
        cxx::BinarySpanWriter writer(*storage);
        if (auto result = DataSchema<T>::encode(value, writer); !result) return cxx::unexpected(result.error());
        const bool is_invalid_output = !writer.good() || writer.size() != *size;
        if (is_invalid_output) return cxx::unexpected(RenderError{kInvalidWire, {writer.size()}});
        auto bytes = OwnedWireBytes::fromOwner(storage, std::span<const std::byte>(*storage));
        if (auto result = validateWire({schema, bytes, attachments}, limits); !result)
        {
            return cxx::unexpected(result.error());
        }
        return bytes;
    }

    template <WireData T>
    [[nodiscard]] RenderResult<T> decodeWire(WireView wire, WireLimits limits) noexcept
    {
        if (auto result = compatibleDataSchemas(DataSchema<T>::describe(), wire.schema); !result)
        {
            return cxx::unexpected(result.error());
        }
        if (auto result = validateWire(wire, limits); !result) return cxx::unexpected(result.error());

        // The common validator accounts for the whole blob tail. A typed decoder
        // must consume the complete fixed record; resolved bytes retain owners.
        cxx::BinaryReader reader(
            wire.bytes.view().first(static_cast<std::size_t>(wire.schema.fixed_size)),
            limits.max_wire_bytes
        );
        auto value = DataSchema<T>::decode(reader, wire);
        if (!value) return cxx::unexpected(value.error());
        if (!reader.requireFullyConsumed()) return cxx::unexpected(RenderError{kInvalidWire, {reader.offset()}});
        return value;
    }
}
