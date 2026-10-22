#pragma once

#include <cstddef>

namespace lux::render
{
    struct TransferOptions final
    {
        std::size_t chunk_bytes = 1024 * 1024;
        std::size_t staging_pages = 4;
        std::size_t pending_bytes = 256 * 1024 * 1024;
        bool measure_gpu = false;         // optional bounded timestamp pairs, never a wait in advance()
        std::size_t window_regions = 256; // Copy regions per finite preparation window.
        std::size_t batch_entries = 128;  // Maximum ready request windows in one submission.
        bool measure_cpu = false;         // CPU clock reads are opt-in, independent of GPU timestamps.
        std::size_t inline_copy_bytes = 16 * 1024; // Maximum contiguous Buffer bytes prepared on RenderThread.
    };

} // namespace lux::render
