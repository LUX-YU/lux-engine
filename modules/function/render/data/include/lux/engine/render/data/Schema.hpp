#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <lux/cxx/core/Schema.hpp>
#include <lux/cxx/binary/ScalarSchema.hpp>
#include <lux/engine/render/core/Error.hpp>

namespace lux::render
{
    struct DataTypeTag;
    using DataTypeId = cxx::SchemaId<DataTypeTag>;
    using DataTypeIdView = cxx::SchemaIdView<DataTypeTag>;

    enum class EDataFieldKind : std::uint8_t
    {
        SCALAR,
        BLOB,
        EXTERNAL
    };

    // Fields are packed in declaration order. References encode uint64 offset/size;
    // external references additionally encode a leading uint32 attachment index.
    struct DataField final
    {
        std::string_view name;
        cxx::EScalarKind scalar = cxx::EScalarKind::U8;
        std::uint32_t count = 1;
        EDataFieldKind kind = EDataFieldKind::SCALAR;
        std::uint32_t reference_alignment = 1;
        std::span<const std::uint64_t> enum_values;
    };

    // DataSchema<T>::describe() returns static metadata. Validators only borrow for
    // the call. This is a wire layout, never sizeof(T)/alignof(T).
    struct DataSchemaDescriptor final
    {
        DataTypeIdView type;
        cxx::SchemaVersion schema_version;
        cxx::SchemaVersion wire_version;
        std::span<const DataField> fields;
        std::uint64_t fixed_size = 0;
        std::uint32_t alignment = 1;
    };

    template <typename T> struct DataSchema;

    inline constexpr auto kInvalidDataSchema = error::errorId("lux.render.data.invalid_schema");
    inline constexpr auto kDataSchemaMismatch = error::errorId("lux.render.data.schema_mismatch");
    inline constexpr auto kInvalidWire = error::errorId("lux.render.data.invalid_wire");
    inline constexpr auto kWireLimit = error::errorId("lux.render.data.wire_limit");
    inline constexpr auto kInvalidDataReference = error::errorId("lux.render.data.invalid_reference");

    [[nodiscard]] RenderResult<void> validateDataSchema(const DataSchemaDescriptor& schema) noexcept;

    [[nodiscard]] RenderResult<void>
    compatibleDataSchemas(const DataSchemaDescriptor& expected, const DataSchemaDescriptor& actual) noexcept;

    [[nodiscard]] std::span<const error::ErrorDescriptor> renderDataErrorDescriptors() noexcept;
}
