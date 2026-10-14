#include "Wire.hpp"
#include <algorithm>

namespace lux::render
{
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


    std::vector<std::byte> operationBytes(const OperationDescriptor& value) noexcept
    {
        cxx::BinaryVectorWriter out;
        writeText(out, value.type.name());
        out.writeUnsigned(value.version.major);
        out.writeUnsigned(value.version.minor);
        out.writeUnsigned(static_cast<std::uint32_t>(value.completion));
        writeBlock(out, schemaBytes(value.payload));
        writeBlock(out, value.reply ? schemaBytes(*value.reply) : std::vector<std::byte>{});
        return std::move(out).take();
    }

    RenderResult<OperationId> bindCapability(std::span<const std::byte> bytes,
        const OperationDescriptor& expected) noexcept
    {
        cxx::BinaryReader in(bytes, wire_limits.max_wire_bytes);
        const auto contract = operationBytes(expected);
        OperationId found;
        std::uint32_t count = 0;
        if (!in.readUnsigned(count) || count > 1024) return cxx::unexpected(RenderError{kProtocolContract});
        for (std::uint32_t i = 0; i < count; ++i)
        {
            OperationId id;
            std::span<const std::byte> description;
            if (!in.readUnsigned(id.index) || !in.readUnsigned(id.gen) || id.isNull() || !readBlock(in, description))
                return cxx::unexpected(RenderError{kProtocolContract});
            if (std::ranges::equal(description, contract))
            {
                if (!found.isNull()) return cxx::unexpected(RenderError{kProtocolContract});
                found = id;
            }
        }
        if (!in.requireFullyConsumed() || found.isNull()) return cxx::unexpected(RenderError{kProtocolContract});
        return found;
    }

    void writeRequest(cxx::BinarySpanWriter& out, const RequestFrame& frame, std::uint32_t size) noexcept
    {
        out.writeUnsigned<std::uint32_t>(0x33514c52);
        out.writeUnsigned(frame.request.index);
        out.writeUnsigned(frame.request.gen);
        out.writeUnsigned(frame.operation.index);
        out.writeUnsigned(frame.operation.gen);
        for (auto value : frame.session) out.writeUnsigned(value);
        out.writeUnsigned(frame.reply_capacity);
        out.writeUnsigned(size);
    }

    RenderResult<RequestFrame> readRequest(std::span<const std::byte> bytes) noexcept
    {
        if (bytes.size() < request_size || bytes.size() > wire_limits.max_wire_bytes)
            return cxx::unexpected(RenderError{kInvalidWire});
        cxx::BinaryReader in(bytes.first(request_size), request_size);
        RequestFrame frame;
        std::uint32_t magic = 0, size = 0;
        in.readUnsigned(magic);
        in.readUnsigned(frame.request.index);
        in.readUnsigned(frame.request.gen);
        in.readUnsigned(frame.operation.index);
        in.readUnsigned(frame.operation.gen);
        for (auto& value : frame.session) in.readUnsigned(value);
        in.readUnsigned(frame.reply_capacity);
        in.readUnsigned(size);
        const bool invalid = magic != 0x33514c52 || frame.request.isNull() ||
            size != bytes.size() - request_size || !in.requireFullyConsumed();
        if (invalid) return cxx::unexpected(RenderError{kInvalidWire});
        frame.payload = bytes.subspan(request_size);
        return frame;
    }

    void writeReply(cxx::BinarySpanWriter& out, const ReplyFrame& frame, std::uint32_t size) noexcept
    {
        out.writeUnsigned<std::uint32_t>(0x33504c52);
        out.writeUnsigned(frame.header.request.index);
        out.writeUnsigned(frame.header.request.gen);
        for (auto value : frame.session) out.writeUnsigned(value);
        out.writeUnsigned(static_cast<std::uint32_t>(frame.header.outcome));
        out.writeUnsigned(frame.header.error.type);
        for (auto value : frame.header.error.args) out.writeUnsigned(value);
        out.writeUnsigned(size);
    }

    RenderResult<ReplyFrame> readReply(const platform::ipc::IpcMessage& message) noexcept
    {
        auto bytes = message.bytes();
        if (bytes.size() < reply_size) return cxx::unexpected(RenderError{kInvalidWire});
        cxx::BinaryReader in(bytes.first(reply_size), reply_size);
        ReplyFrame frame;
        std::uint32_t magic = 0, outcome = 0, size = 0;
        in.readUnsigned(magic);
        in.readUnsigned(frame.header.request.index);
        in.readUnsigned(frame.header.request.gen);
        for (auto& value : frame.session) in.readUnsigned(value);
        in.readUnsigned(outcome);
        in.readUnsigned(frame.header.error.type);
        for (auto& value : frame.header.error.args) in.readUnsigned(value);
        in.readUnsigned(size);
        const bool invalid = magic != 0x33504c52 || frame.header.request.isNull() ||
            outcome > static_cast<unsigned>(EOperationOutcome::OUTCOME_UNKNOWN) ||
            size != bytes.size() - reply_size || !in.requireFullyConsumed();
        if (invalid) return cxx::unexpected(RenderError{kInvalidWire});
        frame.header.outcome = static_cast<EOperationOutcome>(outcome);
        frame.payload = bytes.subspan(reply_size);
        return frame;
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
