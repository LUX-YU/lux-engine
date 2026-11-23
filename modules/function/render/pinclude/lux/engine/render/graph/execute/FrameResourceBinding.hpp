#pragma once

#include <lux/engine/render/resource/ResourceBinding.hpp>
#include <NRI.h>

namespace lux::render::detail
{
    struct FrameResourceBinding final
    {
        [[nodiscard]] ResourceInput input() const noexcept
        {
            return {buffer, buffer_offset, buffer_size, image, addressed_buffers, addressed_revision, queue_uses};
        }

        // NRI returns descriptions owned by the selected native objects.
        void select(nri::Buffer* next_buffer, nri::Texture* next_image, const nri::CoreInterface& core) noexcept
        {
            if (!next_buffer) buffer_description = nullptr;
            else if (buffer != next_buffer || !buffer_description)
                buffer_description = &core.GetBufferDesc(*next_buffer);
            if (!next_image) image_description = nullptr;
            else if (image != next_image || !image_description)
                image_description = &core.GetTextureDesc(*next_image);
            buffer = next_buffer;
            image = next_image;
        }

        const nri::BufferDesc* buffer_description = nullptr;
        const nri::TextureDesc* image_description = nullptr;
        nri::Texture* image = nullptr;
        nri::Descriptor* view = nullptr;
        nri::Buffer* buffer = nullptr;
        std::uint64_t buffer_offset = 0, buffer_size = 0;
        std::span<nri::Buffer* const> addressed_buffers;
        std::uint64_t addressed_revision = 0;
        ResourceQueueUses queue_uses{};
        bool dirty = false;
    };
} // namespace lux::render::detail
