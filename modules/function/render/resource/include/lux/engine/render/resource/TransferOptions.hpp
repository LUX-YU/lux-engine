#pragma once

#include <cstddef>

namespace lux::render
{
    struct TransferOptions final
    {
        std::size_t chunk_bytes = 1024 * 1024;
        std::size_t staging_pages = 4;
        std::size_t pending_bytes = 256 * 1024 * 1024;
        bool measure_gpu = false; // optional bounded timestamp pairs, never a wait in advance()
    };

}
