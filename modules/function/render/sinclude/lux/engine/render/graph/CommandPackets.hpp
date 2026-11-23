#pragma once

#include <NRI.h>
#include <lux/engine/render/graph/GraphResource.hpp>

namespace lux::render::detail
{
    struct PassDescriptorBindings final
    {
        nri::PipelineLayout* layout = nullptr;
        std::uint32_t private_set = 0;
        std::array<std::uint32_t, 3> shared_sets{UINT32_MAX, UINT32_MAX, UINT32_MAX};
        bool operator==(const PassDescriptorBindings&) const noexcept = default;
    };

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

    struct ImageBufferPacket final
    {
        ImageOperand image;
        BufferOperand buffer;
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

    enum class ETransferOpcode : std::uint8_t
    {
        COPY_BUFFER, COPY_IMAGE, UPLOAD, READBACK, ZERO_BUFFER, FILL_STORAGE, CLEAR_COLOR, CLEAR_DEPTH
    };

    struct TransferInstruction final
    {
        ETransferOpcode opcode;
        std::uint32_t operand;
    };

    struct TransferCommands final
    {
        std::uint32_t first, count, first_clear;
        std::uint32_t clear_view_count = 0;
    };

    // Instructions preserve author order. Each operand indexes its opcode's dense typed column.
    struct TransferProgram final
    {
        std::vector<TransferInstruction> instructions;
        std::vector<BufferCopyPacket> buffer_copies;
        std::vector<ImageCopyPacket> image_copies;
        std::vector<ImageBufferPacket> image_buffers;
        std::vector<FillPacket> fills;
        std::vector<ClearPacket> clears;

        [[nodiscard]] std::span<const TransferInstruction> commands(const TransferCommands& pass) const & noexcept
        {
            return std::span{instructions}.subspan(pass.first, pass.count);
        }
        std::span<const TransferInstruction> commands(const TransferCommands&) const && = delete;

        [[nodiscard]] std::span<const ClearPacket> clearCommands(const TransferCommands& pass) const & noexcept
        {
            return std::span{clears}.subspan(pass.first_clear, pass.clear_view_count);
        }
        std::span<const ClearPacket> clearCommands(const TransferCommands&) const && = delete;
    };
}
