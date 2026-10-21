#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace lux::rdesc
{
    inline constexpr std::uint32_t kMaterialShaderAbi = 3;
    inline constexpr std::uint32_t kTextureDefaultWhite = 1u << 0;

    // Cooked Material field names shared by emission and reflected-layout consumers.
    // Offsets and stride still come only from reflection, never from these names.
    inline constexpr std::string_view kMaterialTableName = "uMats";
    inline constexpr std::string_view kMaterialParametersName = "params";
    inline constexpr std::string_view kMaterialTexturesName = "tex";
    inline constexpr std::array<std::string_view, 4>
        kMaterialTextureFields{"representation_index", "resource_index", "sampler_index", "flags"};
} // namespace lux::rdesc
