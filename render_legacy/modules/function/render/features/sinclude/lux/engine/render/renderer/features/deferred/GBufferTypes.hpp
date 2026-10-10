#pragma once
#include <string>

namespace lux::render
{

    /// Shared G-Buffer resource names — used by DeferredGBufferFeature (producer)
    /// and DeferredLightingFeature (consumer) to agree on texture names.
    struct GBufferLayout
    {
        std::string albedo_metallic{"GBufAlbedoMetallic"};
        std::string normal_roughness{"GBufNormalRoughness"};
        std::string emissive_ao{"GBufEmissiveAo"};
    };

} // namespace lux::render
