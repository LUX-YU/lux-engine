#pragma once

#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/description/LightDescription.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace lux::rdesc
{
    struct MeshVisualDescription final
    {
        asset::AssetId mesh{};
        asset::AssetId material{};
        bool visible{true};
        bool cast_shadow{true};
        bool receive_shadow{true};

        friend bool operator==(const MeshVisualDescription&, const MeshVisualDescription&) noexcept = default;
    };

}
