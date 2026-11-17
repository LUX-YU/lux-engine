#include <lux/engine/render/extensions/device/CopyEncoding.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <algorithm>
#include <array>

namespace lux::render::detail
{
    void recordCopy(const nri::CoreInterface& core, nri::CommandBuffer& command, const BufferCopyRegion& copy) noexcept
    {
        core.CmdCopyBuffer(
            command, *copy.destination, copy.destination_offset, *copy.source, copy.source_offset, copy.bytes
        );
    }

    void recordCopy(const nri::CoreInterface& core, nri::CommandBuffer& command, const TextureCopyRegion& copy) noexcept
    {
        for (std::uint32_t mip = 0; mip < copy.mip_count; ++mip)
        {
            auto from = copy.source_region;
            auto to = copy.destination_region;
            from.mipOffset += static_cast<nri::Dim_t>(mip);
            to.mipOffset += static_cast<nri::Dim_t>(mip);
            from.width = to.width = static_cast<nri::Dim_t>(std::max(1u, std::uint32_t(from.width) >> mip));
            from.height = to.height = static_cast<nri::Dim_t>(std::max(1u, std::uint32_t(from.height) >> mip));
            for (std::uint32_t layer = 0; layer < copy.layer_count; ++layer)
            {
                core.CmdCopyTexture(command, *copy.destination, &to, *copy.source, &from);
                ++from.layerOffset;
                ++to.layerOffset;
            }
        }
    }

    void
    recordUpload(
        const nri::CoreInterface& core,
        nri::CommandBuffer& command,
        nri::Texture& destination,
        const nri::TextureRegionDesc& region,
        const nri::Buffer& source,
        const nri::TextureDataLayoutDesc& layout
    ) noexcept
    {
        core.CmdUploadBufferToTexture(command, destination, region, source, layout);
    }

    void
    recordReadback(
        const nri::CoreInterface& core,
        nri::CommandBuffer& command,
        nri::Buffer& destination,
        const nri::TextureDataLayoutDesc& layout,
        const nri::Texture& source,
        const nri::TextureRegionDesc& region
    ) noexcept
    {
        core.CmdReadbackTextureToBuffer(command, destination, layout, source, region);
    }

    void
    recordPreservingCopies(
        const nri::CoreInterface& core,
        nri::CommandBuffer& command,
        std::span<const BufferCopyRegion> copies
    ) noexcept
    {
        for (const auto& copy : copies)
        {
            const std::array<nri::BufferBarrierDesc, 2> barriers{
                {{copy.source,
                  {bufferAccess(core.GetBufferDesc(*copy.source).usage), nri::StageBits::ALL},
                  {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY}},
                 {copy.destination,
                  {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY},
                  {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY}}}
            };
            core.CmdBarrier(command, {nullptr, 0, barriers.data(), static_cast<std::uint32_t>(barriers.size())});
            recordCopy(core, command, copy);
        }
    }
} // namespace lux::render::detail
