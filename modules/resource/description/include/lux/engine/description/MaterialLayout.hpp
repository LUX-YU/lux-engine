#pragma once

#include <array>
#include <string_view>

namespace lux::rdesc
{
    // Cooked Material field names shared by emission and reflected-layout consumers.
    // Offsets and stride still come only from reflection, never from these names.
    inline constexpr std::string_view kMaterialTableName = "uMats";
    inline constexpr std::string_view kMaterialParametersName = "params";
    inline constexpr std::string_view kMaterialTexturesName = "tex";
    inline constexpr std::array<std::string_view, 4> kMaterialTextureFields{
        "representation_index", "resource_index", "aux", "flags"
    };
} // namespace lux::rdesc
