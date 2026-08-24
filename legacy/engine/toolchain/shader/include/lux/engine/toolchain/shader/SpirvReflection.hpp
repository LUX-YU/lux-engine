#pragma once

#include <lux/engine/description/ShaderInfo.hpp>

#include <cstddef>

namespace lux::toolchain
{
    // Toolchain-only SPIR-V reflection. Runtime assets carry the resulting
    // ShaderInfo and never invoke spirv-cross while loading or uploading.
    [[nodiscard]] bool reflectSpirv(
        const void*            bytes,
        std::size_t            byte_count,
        lux::rdesc::ShaderInfo& out
    );
} // namespace lux::toolchain
