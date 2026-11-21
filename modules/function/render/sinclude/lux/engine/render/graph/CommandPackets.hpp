#pragma once

#include <NRI.h>
#include <lux/engine/render/extensions/graph/GraphResource.hpp>

namespace lux::render::detail
{
    struct ImageDispatchExtent final
    {
        std::uint32_t image, mip;
        std::array<std::uint32_t, 2> group_size;
    };

    struct BufferLocation final
    {
        std::uint32_t buffer;
        std::uint64_t offset;
    };

    using VCompiledDispatch = std::variant<nri::DispatchDesc, ImageDispatchExtent, BufferLocation>;

    struct ComputeCommands final
    {
        VCompiledDispatch dispatch;
    };

    // Slots index the executable resource table. Only a read can select a fallback.
    struct BufferOperand final
    {
        std::uint32_t slot, fallback;
        GraphBufferRange range;
    };

    struct ImageOperand final
    {
        std::uint32_t slot, fallback;
        GraphImageRange range;
        std::optional<ImageCopyRegion> region;
        std::uint32_t block_dim, block_bytes;
    };

    struct BufferCopyPacket final { BufferOperand source, destination; };
    struct ImageCopyPacket final { ImageOperand source, destination; };

    enum class EImageBufferCopy { UPLOAD, READBACK };
    struct ImageBufferPacket final
    {
        ImageOperand image;
        BufferOperand buffer;
        EImageBufferCopy direction;
        std::uint32_t row_pitch, slice_pitch;
    };

    struct FillPacket final
    {
        BufferOperand destination;
        std::uint32_t value, descriptor;
        bool sync_before;
    };

    struct ClearPacket final
    {
        ImageOperand destination;
        std::array<float, 4> color;
        float depth;
        std::uint32_t view;
    };

    using VTransferPacket = std::variant<BufferCopyPacket, ImageCopyPacket, ImageBufferPacket, FillPacket, ClearPacket>;

    struct TransferCommands final
    {
        std::vector<VTransferPacket> operations;
        std::uint32_t clear_view_count = 0;
    };
}
