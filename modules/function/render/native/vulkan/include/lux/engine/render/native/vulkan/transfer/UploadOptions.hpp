#pragma once

#include <cstddef>

namespace lux::render::vulkan
{
    struct TextureUploadOptions final
    {
        std::size_t chunk_bytes = 1024 * 1024;
        std::size_t staging_pages = 4;
        std::size_t pending_bytes = 256 * 1024 * 1024;
        // Host-owned, bounded, never inline; accepted callbacks execute exactly once.
        // Worker may ONLY access its supplied byte slice. Host joins before Session/candidate destruction.
        void* user = nullptr;
        bool (*enqueue)(void*, void (*)(void*) noexcept, void*) noexcept = nullptr;
        void (*join)(void*) noexcept = nullptr;
        bool measure_gpu = false; // optional bounded timestamp pairs, never a wait in advance()
    };

}
