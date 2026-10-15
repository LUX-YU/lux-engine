#pragma once

#include <lux/engine/render/features/Program.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/cxx/container/SlotMap.hpp>

namespace lux::render { struct StorageRegistration; }

namespace extension_test
{
    using namespace lux::render;
    struct BufferTag;
    using BufferId = lux::cxx::SlotKey<BufferTag>;
    struct BufferBinding final { BufferId id; };
    struct TestBufferStorage final
    {
        struct Config final
        {
            std::uint32_t capacity = 8;
            bool operator==(const Config&) const noexcept = default;
        };
        static inline constexpr ShaderBinding bindings[]{{1, 0, EDescriptorKind::STORAGE_BUFFER, 1, 16}};
        static const StorageDescriptor description;
        static StorageRegistration registration() noexcept;
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
    template <> struct DataSchema<extension_test::BufferBinding>
    {
        static inline constexpr DataField fields[]{{"index", cxx::EScalarKind::U32}, {"generation", cxx::EScalarKind::U32}};
        static inline constexpr DataSchemaDescriptor schema{
            DataTypeIdView("test.extension.BufferBinding"), {1, 0}, {1, 0}, fields, 8, 4
        };
        static const DataSchemaDescriptor& describe() noexcept { return schema; }
        static RenderResult<std::size_t> wireSize(const extension_test::BufferBinding&) noexcept { return 8; }
        static RenderResult<void> encode(const extension_test::BufferBinding& value, cxx::BinarySpanWriter& out) noexcept
        {
            out.writeUnsigned(value.id.index);
            out.writeUnsigned(value.id.gen);
            return {};
        }
        static RenderResult<extension_test::BufferBinding> decode(cxx::BinaryReader& in, BorrowedWireView) noexcept
        {
            extension_test::BufferBinding value;
            in.readUnsigned(value.id.index);
            in.readUnsigned(value.id.gen);
            return value;
        }
        static RenderResult<extension_test::BufferBinding> decode(cxx::BinaryReader& in, WireView wire) noexcept
        {
            return decode(in, BorrowedWireView{wire.schema, wire.bytes.view(), {}});
        }
    };
}

namespace extension_test
{
    inline const StorageDescriptor TestBufferStorage::description{
        StorageTypeIdView("test.extension.BufferStorage"), {1, 0}, scopeMask(EResourceScope::GLOBAL), nullptr,
        0, 0, bindings, &DataSchema<BufferBinding>::schema
    };
}
