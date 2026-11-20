#pragma once

#include <NRI.h>
#include <cstdint>
#include <span>

namespace lux::render::detail
{
    // Borrowed physical bindings. The supplying object retains their lifetime.
    struct ResourceInput final
    {
        nri::Buffer* buffer = nullptr;
        std::uint64_t offset = 0, size = 0;
        nri::Texture* image = nullptr;
        // Buffers reached indirectly through device addresses in this binding. The immutable
        // slice list changes only with its physical topology, independently of content updates.
        std::span<nri::Buffer* const> addressed_buffers;
        std::uint64_t addressed_revision = 0;
    };
} // namespace lux::render::detail
