#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <lux/engine/render/connection/Connection.hpp>

namespace connection_test
{
    using namespace lux::render;
    struct Value final { std::uint64_t value = 0; };
    struct Payload final { ExternalDataRef external; };

    inline void check(bool value, const char* message)
    {
        if (value) return;
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::abort();
    }

    template <typename T>
    T must(RenderResult<T> result)
    {
        check(static_cast<bool>(result), "expected success");
        return std::move(*result);
    }

    inline void must(RenderResult<void> result) { check(static_cast<bool>(result), "expected success"); }
}

namespace lux::render
{
    template <> struct DataSchema<connection_test::Value> final
    {
        static const DataSchemaDescriptor& describe() noexcept
        {
            static constexpr DataField fields[]{{"value", cxx::EScalarKind::U64}};
            static constexpr DataSchemaDescriptor schema{DataTypeIdView("g3r.value"), {1, 0}, {1, 0}, fields, 8, 8};
            return schema;
        }

        static RenderResult<std::size_t> wireSize(const connection_test::Value&) noexcept { return 8; }

        static RenderResult<void> encode(const connection_test::Value& value, cxx::BinarySpanWriter& writer) noexcept
        {
            writer.writeUnsigned(value.value);
            return {};
        }

        static RenderResult<connection_test::Value> decode(cxx::BinaryReader& reader, WireView) noexcept
        {
            connection_test::Value value;
            reader.readUnsigned(value.value);
            return value;
        }
    };

    template <> struct DataSchema<connection_test::Payload> final
    {
        static const DataSchemaDescriptor& describe() noexcept
        {
            static constexpr DataField fields[]{{"external", cxx::EScalarKind::U8, 1, EDataFieldKind::EXTERNAL}};
            static constexpr DataSchemaDescriptor schema{DataTypeIdView("g3r.payload"), {1, 0}, {1, 0}, fields, 20, 4};
            return schema;
        }

        static RenderResult<std::size_t> wireSize(const connection_test::Payload&) noexcept { return 20; }

        static RenderResult<void> encode(const connection_test::Payload& value, cxx::BinarySpanWriter& writer) noexcept
        {
            writer.writeUnsigned(value.external.attachment_index);
            writer.writeUnsigned(value.external.offset);
            writer.writeUnsigned(value.external.size);
            return {};
        }

        static RenderResult<connection_test::Payload> decode(cxx::BinaryReader& reader, WireView) noexcept
        {
            connection_test::Payload value;
            reader.readUnsigned(value.external.attachment_index);
            reader.readUnsigned(value.external.offset);
            reader.readUnsigned(value.external.size);
            return value;
        }
    };
}
