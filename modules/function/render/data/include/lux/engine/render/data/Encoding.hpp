#pragma once

#include <concepts>
#include <lux/cxx/binary/BinaryReader.hpp>
#include <lux/cxx/binary/BinaryWriter.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/render/data/Schema.hpp>
#include <memory>
#include <utility>
#include <vector>

namespace lux::render
{
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

    struct EncodedDataLimits final
    {
        std::size_t max_encoded_bytes = 0;
        std::size_t max_reference_bytes = 0;
    };

    // Synchronous borrow only. Resolve a reference to retain its actual byte owner.
    // Source owners must keep their backing storage stable and immutable.
    struct RetainableEncodedDataView final
    {
        const DataSchemaDescriptor& schema;
        const lux::cxx::SharedBytes<>& bytes;
        std::span<const lux::cxx::SharedBytes<>> attachments;
    };

    // A synchronous message borrow. It never invents an owner or extends input lifetime.
    // Use RetainableEncodedDataView/resolveExternal to retain an existing owner when data must escape.
    struct EncodedDataView final
    {
        const DataSchemaDescriptor& schema;
        std::span<const std::byte> bytes;
        std::span<const std::span<const std::byte>> attachments;
    };

    [[nodiscard]] RenderResult<void> validateEncodedData(EncodedDataView wire, EncodedDataLimits limits) noexcept;
    // Cold binding must have successfully validated this immutable schema. Only bytes/ranges are checked here.
    [[nodiscard]] RenderResult<void> validateBoundEncodedData(EncodedDataView wire, EncodedDataLimits limits) noexcept;

    [[nodiscard]] RenderResult<lux::cxx::SharedBytes<>> resolveBlob(
        const lux::cxx::SharedBytes<>& owner,
        BlobRef ref,
        std::size_t limit,
        std::uint32_t alignment = 1
    ) noexcept;

    [[nodiscard]] RenderResult<lux::cxx::SharedBytes<>> resolveExternal(
        std::span<const lux::cxx::SharedBytes<>> owners,
        ExternalDataRef ref,
        std::size_t limit,
        std::uint32_t alignment = 1
    ) noexcept;

    [[nodiscard]] RenderResult<void> validateEncodedData(
        RetainableEncodedDataView wire,
        EncodedDataLimits limits
    ) noexcept;

    // Explicit field codecs are required. Trivial copyability alone is not an
    // ownership proof, and there is deliberately no memcpy fallback for any T.
    template <typename T>
    concept EncodableData = requires(
        const T& value,
        cxx::BinarySpanWriter& writer,
        cxx::BinaryReader& reader,
        RetainableEncodedDataView wire
    ) {
        { DataSchema<T>::describe() } noexcept -> std::same_as<const DataSchemaDescriptor&>;
        { DataSchema<T>::encodedSize(value) } noexcept -> std::same_as<RenderResult<std::size_t>>;
        { DataSchema<T>::encode(value, writer) } noexcept -> std::same_as<RenderResult<void>>;
        { DataSchema<T>::decode(reader, wire) } noexcept -> std::same_as<RenderResult<T>>;
    };

    template <typename T>
    concept BorrowedEncodableData = EncodableData<T> && requires(cxx::BinaryReader& reader, EncodedDataView wire) {
        { DataSchema<T>::decode(reader, wire) } noexcept -> std::same_as<RenderResult<T>>;
    };

    template <EncodableData T>
    [[nodiscard]] RenderResult<std::size_t> encodeDataInto(
        const T& value,
        std::span<std::byte> storage,
        std::span<const std::span<const std::byte>> attachments,
        EncodedDataLimits limits
    ) noexcept
    {
        auto size = DataSchema<T>::encodedSize(value);
        if (!size)
        {
            return cxx::unexpected(size.error());
        }
        const bool exceeds_limit = *size > storage.size() || *size > limits.max_encoded_bytes;
        if (exceeds_limit)
        {
            return cxx::unexpected(RenderError{kEncodingLimit, {*size}});
        }
        cxx::BinarySpanWriter writer(storage.first(*size));
        if (auto encoded = DataSchema<T>::encode(value, writer); !encoded)
        {
            return cxx::unexpected(encoded.error());
        }
        if (!writer.good() || writer.size() != *size)
        {
            return cxx::unexpected(RenderError{kInvalidEncoding});
        }
        if (auto valid = validateEncodedData(
                EncodedDataView{DataSchema<T>::describe(), storage.first(*size), attachments},
                limits
            );
            !valid)
        {
            return cxx::unexpected(valid.error());
        }
        return *size;
    }

    template <BorrowedEncodableData T>
    [[nodiscard]] RenderResult<T> decodeDataBytes(EncodedDataView wire, EncodedDataLimits limits) noexcept
    {
        if (auto same = compatibleDataSchemas(DataSchema<T>::describe(), wire.schema); !same)
        {
            return cxx::unexpected(same.error());
        }
        if (auto valid = validateEncodedData(wire, limits); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        cxx::BinaryReader reader(
            wire.bytes.first(static_cast<std::size_t>(wire.schema.fixed_size)),
            limits.max_encoded_bytes
        );
        auto value = DataSchema<T>::decode(reader, wire);
        if (!value)
        {
            return cxx::unexpected(value.error());
        }
        if (!reader.requireFullyConsumed())
        {
            return cxx::unexpected(RenderError{kInvalidEncoding});
        }
        return value;
    }

    template <EncodableData T>
    [[nodiscard]] RenderResult<lux::cxx::SharedBytes<>> encodeData(
        const T& value,
        std::span<const lux::cxx::SharedBytes<>> attachments,
        EncodedDataLimits limits
    ) noexcept
    {
        const auto& schema = DataSchema<T>::describe();
        if (auto result = validateDataSchema(schema); !result)
        {
            return cxx::unexpected(result.error());
        }
        auto size = DataSchema<T>::encodedSize(value);
        if (!size)
        {
            return cxx::unexpected(size.error());
        }
        if (*size > limits.max_encoded_bytes)
        {
            return cxx::unexpected(RenderError{kEncodingLimit, {*size}});
        }
        if (*size < schema.fixed_size)
        {
            return cxx::unexpected(RenderError{kInvalidEncoding, {*size}});
        }

        // One retained backing allocation; no copy when publishing owned bytes.
        auto storage = std::make_shared<std::vector<std::byte>>(*size);
        cxx::BinarySpanWriter writer(*storage);
        if (auto result = DataSchema<T>::encode(value, writer); !result)
        {
            return cxx::unexpected(result.error());
        }
        const bool is_invalid_output = !writer.good() || writer.size() != *size;
        if (is_invalid_output)
        {
            return cxx::unexpected(RenderError{kInvalidEncoding, {writer.size()}});
        }
        auto bytes = lux::cxx::SharedBytes<>::fromOwner(storage, std::span<const std::byte>(*storage));
        if (auto result = validateEncodedData({schema, bytes, attachments}, limits); !result)
        {
            return cxx::unexpected(result.error());
        }
        return bytes;
    }

    template <EncodableData T>
    [[nodiscard]] RenderResult<T> decodeData(RetainableEncodedDataView wire, EncodedDataLimits limits) noexcept
    {
        if (auto result = compatibleDataSchemas(DataSchema<T>::describe(), wire.schema); !result)
        {
            return cxx::unexpected(result.error());
        }
        if (auto result = validateEncodedData(wire, limits); !result)
        {
            return cxx::unexpected(result.error());
        }

        // The common validator accounts for the whole blob tail. A typed decoder
        // must consume the complete fixed record; resolved bytes retain owners.
        cxx::BinaryReader reader(
            wire.bytes.view().first(static_cast<std::size_t>(wire.schema.fixed_size)),
            limits.max_encoded_bytes
        );
        auto value = DataSchema<T>::decode(reader, wire);
        if (!value)
        {
            return cxx::unexpected(value.error());
        }
        if (!reader.requireFullyConsumed())
        {
            return cxx::unexpected(RenderError{kInvalidEncoding, {reader.offset()}});
        }
        return value;
    }
} // namespace lux::render
