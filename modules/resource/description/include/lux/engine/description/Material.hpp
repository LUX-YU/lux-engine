#pragma once

#include <lux/engine/description/MaterialProgramSet.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace lux::rdesc
{
    struct MaterialDescription final
    {
        MaterialProgramSet programs;
        std::vector<lux::asset::AssetId> texture_slot_ids;
    };

    struct MaterialInstanceDescription final
    {
        struct ParameterOverride final
        {
            std::uint32_t slot;
            std::array<float, 4> value;
        };

        struct TextureOverride final
        {
            std::uint32_t slot;
            lux::asset::AssetId texture;
        };

        lux::asset::AssetId parent_material_id;
        std::vector<ParameterOverride> parameters;
        std::vector<TextureOverride> textures;
        std::uint32_t render_state_override{};
        EAlphaMode alpha_mode{EAlphaMode::OPAQUE_SURFACE};
        bool double_sided{};
    };
} // namespace lux::rdesc
