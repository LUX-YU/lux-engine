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
    struct BufferBinding final
    {
        BufferId id;
        bool operator==(const BufferBinding&) const noexcept = default;
    };
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


}

namespace extension_test
{
    inline const StorageDescriptor TestBufferStorage::description{
        StorageNameView("test.extension.BufferStorage"),
        0, 0, ConfigValue::typeOf<BufferBinding>()
    };
}
