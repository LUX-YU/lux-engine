#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/render/transport/Operation.hpp>

namespace transport_test
{
    struct LUX_OP(lane=reply, name="test.transport.ack.v1") Ack final
    {
        std::uint64_t value{};
    };

    struct LUX_OP(lane=program, kind=stream, name="test.transport.value.v1") Value final
    {
        std::uint64_t value{};
    };

    struct LUX_OP(lane=program, kind=bulk, name="test.transport.bulk.v1") Bulk final
    {
        std::uint64_t value{};
    };

    struct LUX_OP(lane=program, kind=blob, name="test.transport.blob.v1") Blob final
    {
        std::uint32_t revision{};
        LUX_OP_BLOB() lux::render::BlobRef bytes;
    };

    struct LUX_OP(lane=control, kind=resource, reply="transport_test::Ack", name="test.transport.ping.v1") Ping final
    {
        std::uint64_t value{};
    };

    struct LUX_OP(lane=upload, kind=stream, name="test.transport.upload.v1") Upload final
    {
        std::uint64_t value{};
        lux::render::ExternalDataRef bytes;
    };

    struct LUX_OP(lane=upload, kind=resource, reply="transport_test::Ack", name="test.transport.load.v1") Load final
    {
        std::uint64_t value{};
    };
}
