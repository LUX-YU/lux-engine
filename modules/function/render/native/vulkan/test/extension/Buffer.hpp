#pragma once

#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/data/ConfigValue.hpp>
#include <lux/engine/render/data/Encoding.hpp>

#include <lux/engine/render/protocol/Protocol.hpp>

#include <lux/engine/render/graph/Program.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/data/Payload.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <tuple>

namespace lux::render
{
}

namespace extension_test
{
    using namespace lux::render;
    struct BufferTag;
    using BufferId = StorageResourceId<BufferTag>;
    struct BufferBinding final { BufferId id; };
    struct SetBuffer final
    {
        static constexpr std::string_view name = "test.extension.SetBuffer";
        struct Request final { BufferId id; RenderPayload source; };
        using Response = BufferBinding;
    };
    struct RemoveBuffer final
    {
        static constexpr std::string_view name = "test.extension.RemoveBuffer";
        using Request = BufferBinding;
        using Response = Ack;
    };
    struct ReadBuffer final
    {
        static constexpr std::string_view name = "test.extension.ReadBuffer";
        using Request = BufferBinding;
        struct Response final { RenderPayload pixels; };
    };
    struct TestBufferStorage final
    {
        struct Config final
        {
            std::uint32_t capacity = 8;
            bool operator==(const Config&) const noexcept = default;
        };
        static inline constexpr ShaderBinding bindings[]{{1, 0, EDescriptorKind::STORAGE_BUFFER, 1, 16}};
        static const StorageDescriptor description;
        struct Ops final { bool (*exists)(const void*, BufferId) noexcept; };
        class Access final
        {
        public:
            bool exists(BufferId id) const noexcept { return ops_->exists(native_, id); }
        private:
            friend class lux::render::ResourceRegistry;
            Access(void* native, const Ops& ops, bool) noexcept : native_(native), ops_(&ops) {}
            void* native_;
            const Ops* ops_;
        };
    };
}

namespace lux::render
{

    template <> struct CommandValueTraits<extension_test::SetBuffer::Request> final
    {
        static RenderResult<std::size_t> retainedBytes(const extension_test::SetBuffer::Request& value) noexcept
        {
            const auto bytes = cxx::checkedAdd(sizeof(value), value.source.retainedBytes());
            if (!bytes) return cxx::unexpected(RenderError{kEncodingLimit});
            return *bytes;
        }
    };

    template <> struct CommandValueTraits<extension_test::ReadBuffer::Response> final
    {
        static RenderResult<std::size_t> retainedBytes(const extension_test::ReadBuffer::Response& value) noexcept
        {
            const auto bytes = cxx::checkedAdd(sizeof(value), value.pixels.retainedBytes());
            if (!bytes) return cxx::unexpected(RenderError{kEncodingLimit});
            return *bytes;
        }
    };

    template <> struct DataSchema<extension_test::BufferBinding>
    {
        static inline constexpr DataField fields[]{
            {"storage_index", cxx::EScalarKind::U32}, {"storage_generation", cxx::EScalarKind::U32},
            {"index", cxx::EScalarKind::U32}, {"generation", cxx::EScalarKind::U32}
        };
        static inline constexpr DataSchemaDescriptor schema{
            DataTypeIdView("test.extension.BufferBinding"), {2, 0}, {2, 0}, fields, 16, 4
        };
        static const DataSchemaDescriptor& describe() noexcept { return schema; }
        static RenderResult<std::size_t> encodedSize(const extension_test::BufferBinding&) noexcept { return 16; }
        static RenderResult<void> encode(const extension_test::BufferBinding& value, cxx::BinarySpanWriter& out) noexcept
        {
            out.writeUnsigned(value.id.storage.index);
            out.writeUnsigned(value.id.storage.gen);
            out.writeUnsigned(value.id.local.index);
            out.writeUnsigned(value.id.local.gen);
            return {};
        }
        static RenderResult<extension_test::BufferBinding> decode(cxx::BinaryReader& in, EncodedDataView) noexcept
        {
            extension_test::BufferBinding value;
            in.readUnsigned(value.id.storage.index);
            in.readUnsigned(value.id.storage.gen);
            in.readUnsigned(value.id.local.index);
            in.readUnsigned(value.id.local.gen);
            return value;
        }
        static RenderResult<extension_test::BufferBinding> decode(cxx::BinaryReader& in, RetainableEncodedDataView wire) noexcept
        {
            return decode(in, EncodedDataView{wire.schema, wire.bytes.view(), {}});
        }
    };
}

namespace extension_test
{
    inline const StorageDescriptor TestBufferStorage::description{
        StorageNameView("test.extension.BufferStorage"),
        0, 0, bindings, &DataSchema<BufferBinding>::schema
    };
}
