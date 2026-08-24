#pragma once

#include <lux/engine/authoring/assets/visibility.h>
#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>
#include <lux/engine/resource/asset/Asset.hpp>

#include <cstdint>
#include <string>

namespace lux::authoring
{
    /// Auxiliary payload owned by Authoring. Runtime asset codecs preserve this
    /// block opaquely, while cooked export strips AUTHORING_ONLY payloads.
    inline constexpr lux::asset::payload_tag_t kMaterialGraphPayloadTag =
        0x4850524754414D4Cull; // "LMATGRPH" in little-endian byte order

    [[nodiscard]] LUX_ENGINE_AUTHORING_ASSETS_PUBLIC bool
    readMaterialGraph(
        const lux::asset::LuxAsset& asset,
        lux::rdesc::MaterialGraph&  graph,
        std::string*                error = nullptr
    ) noexcept;

    LUX_ENGINE_AUTHORING_ASSETS_PUBLIC void attachMaterialGraph(
        lux::asset::LuxAsset&             asset,
        const lux::rdesc::MaterialGraph&  graph
    );
} // namespace lux::authoring
