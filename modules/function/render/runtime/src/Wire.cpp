#include "Wire.hpp"
#include <algorithm>
#include <limits>

namespace lux::render
{
    using runtime_detail::Record;

    const DataSchemaDescriptor& DataSchema<Record>::describe() noexcept
    {
        static constexpr DataField fields[]{
            {"session", cxx::EScalarKind::U64, 2}, {"request", cxx::EScalarKind::U32, 2},
            {"operation", cxx::EScalarKind::U32, 2}, {"arguments", cxx::EScalarKind::U32, 16},
            {"color", cxx::EScalarKind::F32, 4}, {"reply_capacity", cxx::EScalarKind::U64},
            {"error", cxx::EScalarKind::U64, 4}, {"body", cxx::EScalarKind::U8, 1, EDataFieldKind::BLOB}
        };
        static const DataSchemaDescriptor schema{
            DataTypeIdView("lux.render.runtime.control.v1"), {1, 0}, {1, 0}, fields, runtime_detail::record_size, 8
        };
        return schema;
    }

    RenderResult<std::size_t> DataSchema<Record>::wireSize(const Record& value) noexcept
    {
        if (value.body.size() > runtime_detail::wire_limits.max_wire_bytes - runtime_detail::record_size)
            return cxx::unexpected(RenderError{kWireLimit});
        return runtime_detail::record_size + value.body.size();
    }

    RenderResult<void> DataSchema<Record>::encode(const Record& value, cxx::BinarySpanWriter& out) noexcept
    {
        for (auto x : value.session) out.writeUnsigned(x);
        for (auto x : {value.request.index, value.request.gen, value.operation.index, value.operation.gen})
            out.writeUnsigned(x);
        for (auto x : value.args) out.writeUnsigned(x);
        for (auto x : value.color) out.writeFloat(x);
        out.writeUnsigned(value.reply_capacity);
        out.writeUnsigned(value.error.type);
        for (auto x : value.error.args) out.writeUnsigned(x);
        out.writeUnsigned<std::uint64_t>(runtime_detail::record_size);
        out.writeUnsigned<std::uint64_t>(value.body.size());
        out.writeBytes(value.body.view());
        return {};
    }

    RenderResult<Record> DataSchema<Record>::decode(cxx::BinaryReader& in, WireView wire) noexcept
    {
        std::span<const std::byte> body;
        auto value = runtime_detail::decodeRecord(wire.bytes.view(), body);
        if (!value) return cxx::unexpected(value.error());
        // Keep the typed G2 entry point's owned-blob semantics unchanged.
        value->body = wire.bytes.subspan(runtime_detail::record_size, body.size());
        std::span<const std::byte> consumed;
        in.readBytes(runtime_detail::record_size, consumed);
        return value;
    }

    std::span<const error::ErrorDescriptor> renderRuntimeErrorDescriptors() noexcept
    {
        static constexpr error::ErrorDescriptor descriptors[]{
            {"lux.render.runtime.state", "Invalid runtime state"},
            {"lux.render.runtime.capacity", "Runtime capacity exhausted"},
            {"lux.render.runtime.identity", "Stale or foreign runtime identity"},
            {"lux.render.runtime.unsupported", "Unsupported runtime operation"}
        };
        return descriptors;
    }
}

namespace lux::render::runtime_detail
{
    RenderResult<Record> decodeRecord(std::span<const std::byte> bytes,
        std::span<const std::byte>& body) noexcept
    {
        const bool is_invalid_size = bytes.size() < record_size || bytes.size() > wire_limits.max_wire_bytes;
        if (is_invalid_size) return cxx::unexpected(RenderError{kInvalidWire});
        cxx::BinaryReader in(bytes.first(record_size), wire_limits.max_wire_bytes);
        Record value;
        for (auto& x : value.session) in.readUnsigned(x);
        for (auto* x : {&value.request.index, &value.request.gen, &value.operation.index, &value.operation.gen})
            in.readUnsigned(*x);
        for (auto& x : value.args) in.readUnsigned(x);
        for (auto& x : value.color) in.readFloat(x);
        in.readUnsigned(value.reply_capacity);
        in.readUnsigned(value.error.type);
        for (auto& x : value.error.args) in.readUnsigned(x);
        BlobRef range;
        in.readUnsigned(range.offset);
        in.readUnsigned(range.size);
        if (range.offset != record_size || range.size != bytes.size() - record_size)
            return cxx::unexpected(RenderError{kInvalidWire});
        if (!in.requireFullyConsumed() || value.request.isNull())
            return cxx::unexpected(RenderError{kInvalidWire});
        body = bytes.subspan(record_size);
        return value;
    }

    OwnedWireBytes own(std::vector<std::byte> bytes) noexcept
    {
        auto owner = std::make_shared<std::vector<std::byte>>(std::move(bytes));
        return OwnedWireBytes::fromOwner(owner, *owner);
    }

    void writeText(cxx::BinaryVectorWriter& out, std::string_view value) noexcept
    {
        out.writeUnsigned<std::uint64_t>(value.size());
        out.writeString(value);
    }

    bool readText(cxx::BinaryReader& in, std::string_view& value) noexcept
    {
        std::uint64_t size = 0;
        return in.readUnsigned(size) && size <= 1024 && in.readString(static_cast<std::size_t>(size), value);
    }

    void writeBlock(cxx::BinaryVectorWriter& out, std::span<const std::byte> value) noexcept
    {
        out.writeUnsigned<std::uint64_t>(value.size());
        out.writeBytes(value);
    }

    bool readBlock(cxx::BinaryReader& in, std::span<const std::byte>& value) noexcept
    {
        std::uint64_t size = 0;
        return in.readUnsigned(size) && size <= wire_limits.max_wire_bytes &&
            in.readBytes(static_cast<std::size_t>(size), value);
    }

    std::vector<std::byte> schemaBytes(const DataSchemaDescriptor& schema) noexcept
    {
        cxx::BinaryVectorWriter out;
        writeText(out, schema.type.name());
        for (auto x : {schema.schema_version.major, schema.schema_version.minor,
             schema.wire_version.major, schema.wire_version.minor}) out.writeUnsigned(x);
        out.writeUnsigned(schema.fixed_size);
        out.writeUnsigned(schema.alignment);
        out.writeUnsigned<std::uint64_t>(schema.fields.size());
        for (const auto& field : schema.fields)
        {
            writeText(out, field.name);
            out.writeUnsigned(static_cast<std::uint32_t>(field.scalar));
            out.writeUnsigned(field.count);
            out.writeUnsigned(static_cast<std::uint32_t>(field.kind));
            out.writeUnsigned(field.reference_alignment);
            out.writeUnsigned<std::uint64_t>(field.enum_values.size());
            for (auto x : field.enum_values) out.writeUnsigned(x);
        }
        return std::move(out).take();
    }

    OperationDescriptor descriptor(ERenderCommand operation) noexcept
    {
        static constexpr std::array names{
            "render.ready", "render.scene.create", "render.scene.remove", "render.view.create", "render.view.remove",
            "render.target.create", "render.target.resize", "render.effect.create", "render.effect.remove",
            "render.attach", "render.detach", "render.readback", "render.stop"
        };
        const auto completion = operation == ERenderCommand::READBACK ? ECompletionContract::RESULT_AVAILABLE :
            ECompletionContract::STATE_COMMITTED;
        return operationDescriptor<Record, Record>(names[static_cast<std::size_t>(operation)], {1, 0}, completion);
    }

    std::vector<std::byte> capabilities(std::span<const OperationId> operations) noexcept
    {
        cxx::BinaryVectorWriter out;
        for (auto id : operations) { out.writeUnsigned(id.index); out.writeUnsigned(id.gen); }
        for (std::size_t i = 0; i < operation_count; ++i)
        {
            const auto op = descriptor(static_cast<ERenderCommand>(i));
            writeText(out, op.type.name());
            out.writeUnsigned(op.version.major);
            out.writeUnsigned(op.version.minor);
            out.writeUnsigned(static_cast<std::uint32_t>(op.completion));
            writeBlock(out, schemaBytes(op.payload));
            writeBlock(out, schemaBytes(*op.reply));
        }
        return std::move(out).take();
    }

    RenderResult<std::array<OperationId, operation_count>> bindCapabilities(std::span<const std::byte> bytes) noexcept
    {
        cxx::BinaryReader in(bytes, wire_limits.max_wire_bytes);
        std::array<OperationId, operation_count> ids{};
        for (auto& id : ids)
            if (!in.readUnsigned(id.index) || !in.readUnsigned(id.gen))
                return cxx::unexpected(RenderError{kProtocolContract});
        const auto expected = capabilities(ids);
        if (!std::ranges::equal(bytes, expected)) return cxx::unexpected(RenderError{kProtocolContract});
        return ids;
    }

    RenderResult<std::vector<std::byte>> selections(std::span<const FeatureSelection> features) noexcept
    {
        if (features.size() > max_features) return cxx::unexpected(RenderError{kRuntimeCapacity});
        cxx::BinaryVectorWriter out;
        out.writeUnsigned<std::uint32_t>(static_cast<std::uint32_t>(features.size()));
        for (const auto& feature : features)
        {
            const bool invalid = feature.type.empty() || feature.type.size() > 1024 || feature.instance.size() > 1024 ||
                feature.config.size() > wire_limits.max_wire_bytes || (!feature.config_schema && !feature.config.empty());
            if (invalid) return cxx::unexpected(RenderError{kInvalidWire});
            writeText(out, feature.type);
            writeText(out, feature.instance);
            out.writeUnsigned(feature.version.major);
            out.writeUnsigned(feature.version.minor);
            if (feature.config_schema)
            {
                if (auto valid = validateDataSchema(*feature.config_schema); !valid) return cxx::unexpected(valid.error());
                writeBlock(out, schemaBytes(*feature.config_schema));
            }
            else writeBlock(out, {});
            writeBlock(out, feature.config.view());
        }
        return std::move(out).take();
    }
}
