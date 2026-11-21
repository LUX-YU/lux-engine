#pragma once

#include <NRI.h>
#include <lux/engine/render/visibility.h>
#include <span>

namespace lux::render::detail
{
    // Qualified physical ranges. The caller owns access transitions and keeps every resource alive.
    // These values own neither a command buffer nor staging, graph resources or queue state.
    struct BufferCopyRegion final
    {
        nri::Buffer* source;
        nri::Buffer* destination;
        std::uint64_t source_offset, destination_offset, bytes;
    };

    struct TextureCopyRegion final
    {
        const nri::Texture* source;
        nri::Texture* destination;
        nri::TextureRegionDesc source_region, destination_region;
        std::uint32_t mip_count = 1, layer_count = 1;
    };

    // Encode into the caller's exclusive CB only; no Begin/End, allocation, submission or waits.
    LUX_RENDER_PUBLIC void recordCopy(const nri::CoreInterface&, nri::CommandBuffer&, const BufferCopyRegion&) noexcept;
    LUX_RENDER_PUBLIC void
    recordCopy(const nri::CoreInterface&, nri::CommandBuffer&, const TextureCopyRegion&) noexcept;

    struct UploadRegion final
    {
        nri::Texture& destination;
        nri::TextureRegionDesc region;
        const nri::Buffer& source;
        nri::TextureDataLayoutDesc layout;
    };

    struct ReadbackRegion final
    {
        nri::Buffer& destination;
        nri::TextureDataLayoutDesc layout;
        const nri::Texture& source;
        nri::TextureRegionDesc region;
    };

    LUX_RENDER_PUBLIC void recordUpload(const nri::CoreInterface&, nri::CommandBuffer&, const UploadRegion&) noexcept;
    LUX_RENDER_PUBLIC void recordReadback(const nri::CoreInterface&, nri::CommandBuffer&, const ReadbackRegion&) noexcept;

    // Professional buffer growth preserves source contents. Destinations are already COPY_DESTINATION;
    // final access transitions still belong to the preparation/consumer, not this recording helper.
    LUX_RENDER_PUBLIC void
    recordPreservingCopies(const nri::CoreInterface&, nri::CommandBuffer&, std::span<const BufferCopyRegion>) noexcept;
} // namespace lux::render::detail
