#pragma once

#include <concepts>
#include <cstddef>
#include <lux/engine/description/DataLayout.hpp>
#include <lux/engine/render/core/Descriptors.hpp>
#include <optional>
#include <span>
#include <string_view>

namespace lux::render
{
    enum class EDataScope
    {
        RUNTIME,
        SCENE,
        VIEW
    };
    enum class EDataAuthority
    {
        CPU,
        GPU,
        EXTERNAL
    };

    struct DataField
    {
        std::string_view name, cpp_type;
        bool operator==(const DataField&) const noexcept = default;
    };

    struct DataCpuRepresentation
    {
        std::size_t size, alignment;
        bool movable, copyable, trivially_copyable;
        bool operator==(const DataCpuRepresentation&) const noexcept = default;
    };

    struct DataWireRepresentation
    {
        std::string_view codec;
        std::uint32_t version;
        bool operator==(const DataWireRepresentation&) const noexcept = default;
    };

    // No registry/owner. Callers must keep borrowed metadata alive through validation.
    struct DataSchemaDescriptor
    {
        RenderDataTypeId id;
        std::string_view canonical_name;
        std::uint32_t schema_version;
        EDataScope scope;
        EDataAuthority authority;
        std::span<const DataField> fields;
        std::optional<DataCpuRepresentation> cpu;
        std::optional<DataWireRepresentation> wire;
        std::optional<rdesc::DataGpuLayout> gpu;
    };

    template <class T> struct DataSchema;

    template <class T>
    concept DescribedData = requires {
        typename DataSchema<T>::Value;
        requires std::same_as<typename DataSchema<T>::Value, T>;
        { DataSchema<T>::descriptor() } noexcept -> std::same_as<DataSchemaDescriptor>;
        requires DataSchema<T>::validated;
        requires DataSchema<T>::descriptor().schema_version > 0;
        requires !DataSchema<T>::descriptor().canonical_name.empty();
        requires !DataSchema<T>::descriptor().fields.empty();
        requires DataSchema<T>::descriptor().id == renderDataTypeId(DataSchema<T>::descriptor().canonical_name);
    };

    template <class T>
    concept WireData = DescribedData<T> && requires {
        requires DataSchema<T>::descriptor().wire.has_value();
        requires DataSchema<T>::descriptor().wire->version > 0;
        requires !DataSchema<T>::descriptor().wire->codec.empty();
    };

    template <class T>
    concept GpuData = DescribedData<T> && requires {
        { DataSchema<T>::gpuLayout() } noexcept -> std::same_as<rdesc::DataGpuLayout>;
        requires DataSchema<T>::descriptor().gpu.has_value();
        requires DataSchema<T>::gpuLayout().size > 0;
        requires !DataSchema<T>::gpuLayout().fields.empty();
    };

    template <class T>
    concept CpuAuthoredGpuData = GpuData<T> && requires(const T& value, std::span<std::byte> output) {
        requires DataSchema<T>::descriptor().authority == EDataAuthority::CPU;
        { DataSchema<T>::pack(value, output) } noexcept -> std::same_as<RenderResult<void>>;
    };

    [[nodiscard]] RenderResult<void> validateDataSchema(const DataSchemaDescriptor& schema) noexcept;
    [[nodiscard]] RenderResult<void> compatibleDataSchemas(
        const DataSchemaDescriptor& existing,
        const DataSchemaDescriptor& candidate
    ) noexcept;
} // namespace lux::render
