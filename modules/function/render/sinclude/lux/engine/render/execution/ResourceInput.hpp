#pragma once

#include <NRI.h>
#include <cstddef>
#include <cstdint>
#include <lux/engine/description/ShaderBlock.hpp>
#include <span>

namespace lux::render::detail
{
    // Borrowed physical data and parameter bytes. The supplying object retains their lifetime.
    struct ResourceInput final
    {
        nri::Buffer* buffer = nullptr;
        std::uint64_t offset = 0, size = 0;
        std::span<const std::byte> parameters;
        const rdesc::ShaderBlock* parameter_layout = nullptr;
    };
} // namespace lux::render::detail
