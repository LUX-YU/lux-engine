#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <lux/engine/render/transport/Transport.hpp>

namespace transport_test
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
    template <> struct DataSchema<transport_test::Value> final
    {
        static const DataSchemaDescriptor& describe() noexcept
        {
            static constexpr DataField fields[]{{"value", cxx::EScalarKind::U64}};
            static constexpr DataSchemaDescriptor schema{DataTypeIdView("g3.value"), {1, 0}, {1, 0}, fields, 8, 8};
            return schema;
        }

        static RenderResult<std::size_t> wireSize(const transport_test::Value&) noexcept { return 8; }

        static RenderResult<void> encode(const transport_test::Value& value, cxx::BinarySpanWriter& writer) noexcept
        {
            writer.writeUnsigned(value.value);
            return {};
        }

        static RenderResult<transport_test::Value> decode(cxx::BinaryReader& reader, WireView) noexcept
        {
            transport_test::Value value;
            reader.readUnsigned(value.value);
            return value;
        }
    };

    template <> struct DataSchema<transport_test::Payload> final
    {
        static const DataSchemaDescriptor& describe() noexcept
        {
            static constexpr DataField fields[]{{"external", cxx::EScalarKind::U8, 1, EDataFieldKind::EXTERNAL}};
            static constexpr DataSchemaDescriptor schema{DataTypeIdView("g3.payload"), {1, 0}, {1, 0}, fields, 20, 4};
            return schema;
        }

        static RenderResult<std::size_t> wireSize(const transport_test::Payload&) noexcept { return 20; }

        static RenderResult<void> encode(const transport_test::Payload& value, cxx::BinarySpanWriter& writer) noexcept
        {
            writer.writeUnsigned(value.external.attachment_index);
            writer.writeUnsigned(value.external.offset);
            writer.writeUnsigned(value.external.size);
            return {};
        }

        static RenderResult<transport_test::Payload> decode(cxx::BinaryReader& reader, WireView) noexcept
        {
            transport_test::Payload value;
            reader.readUnsigned(value.external.attachment_index);
            reader.readUnsigned(value.external.offset);
            reader.readUnsigned(value.external.size);
            return value;
        }
    };
}

namespace transport_test
{
    inline OwnedWireBytes encode(std::uint64_t value)
    {
        return must(encodeWire(Value{value}, {}, {8, 0}));
    }

    inline TerminalReply terminal(RenderRequest& request)
    {
        auto value = must(request.poll());
        check(value.has_value(), "terminal ready");
        return std::move(*value);
    }

    struct HandlerState final
    {
        std::vector<std::uint64_t> seen;
        std::optional<ReplyPromise> deferred;
        OwnedWireBytes answer = encode(42);
        bool defer = false;
        bool drop = false;
    };

    inline void handler(void* opaque, ReplyPromise promise) noexcept
    {
        auto& state = *static_cast<HandlerState*>(opaque);
        auto value = must(decodeWire<Value>(must(promise.input()), {8, 0}));
        state.seen.push_back(value.value);
        if (state.defer) { state.deferred.emplace(std::move(promise)); return; }
        if (state.drop) return;
        if (value.value == 13) { must(promise.fail(RenderError{kTransportContract})); return; }
        must(promise.success(DataSchema<Value>::describe(), state.answer));
    }

    inline void voidHandler(void*, ReplyPromise promise) noexcept { must(promise.success()); }
}
