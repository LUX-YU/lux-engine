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

namespace extension_test
{
    using namespace lux::render;
    struct BufferTag;
    using BufferId = StorageResourceId<BufferTag>;
    struct BufferBinding final
    {
        BufferId id;
        std::array<float, 4> gain{1, 1, 1, 1};
        bool operator==(const BufferBinding&) const noexcept = default;
    };
    struct SetBuffer final
    {
        static constexpr std::string_view name = "test.extension.SetBuffer";
        struct Request final { BufferId id; RenderPayload source; };
        using Response = RenderResult<BufferBinding>;
    };
    struct RemoveBuffer final
    {
        static constexpr std::string_view name = "test.extension.RemoveBuffer";
        using Request = BufferBinding;
        using Response = RenderResult<Ack>;
    };
    struct ReadBuffer final
    {
        static constexpr std::string_view name = "test.extension.ReadBuffer";
        using Request = BufferBinding;
        struct ResponseValue final { RenderPayload pixels; };
        using Response = RenderResult<ResponseValue>;
    };
    struct TestBufferStorage final
    {
        struct Config final
        {
            std::uint32_t capacity = 8;
            std::uint32_t bytes = 16;
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

namespace extension_test
{
    inline const StorageDescriptor TestBufferStorage::description{
        StorageNameView("test.extension.BufferStorage"),
        0, 0, cppType<BufferBinding>()
    };
}
