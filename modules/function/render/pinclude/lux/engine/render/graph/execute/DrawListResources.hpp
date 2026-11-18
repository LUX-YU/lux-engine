#pragma once

#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>

namespace lux::render::detail
{
    // Owned as a value by the existing FrameResources. Session cleanup releases the
    // physical buffers; these fields do not create another resource owner or scope.
    struct DrawListResources final
    {
        nri::Buffer* vertices = nullptr;
        nri::Buffer* indices = nullptr;
        std::uint64_t vertex_capacity = 0, index_capacity = 0;
        RasterDrawList draws;
        // Reused only while this FIF slice is writable. Clear borrowed sources at completion.
        std::vector<std::byte> vertex_data, index_data;
        std::vector<BufferWrite> upload_writes;
    };
} // namespace lux::render::detail
