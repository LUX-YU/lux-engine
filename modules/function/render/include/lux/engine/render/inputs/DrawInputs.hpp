#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace lux::render
{
    enum class EIndexType : std::uint8_t
    {
        UINT16,
        UINT32
    };

    struct IndexedDrawRange final
    {
        std::uint32_t first_index = 0, index_count = 0;
        std::int32_t vertex_offset = 0;
        std::uint32_t x = 0, y = 0, width = 0, height = 0;
        std::uint32_t selection = 0;
    };

    // Borrowed through the upload terminal. Indices name vertices in the supplied interleaved stream;
    // their contents are authored geometry, just as for an ordinary DrawIndexed resource.
    struct DrawListData final
    {
        std::span<const std::byte> vertices, indices, parameters;
        std::span<const IndexedDrawRange> draws;
    };

} // namespace lux::render
