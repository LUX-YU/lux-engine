#include <lux/engine/authoring/assets/MaterialGraphDocument.hpp>
#include <lux/engine/authoring/assets/MaterialGraphCodec.hpp>

#include <span>

namespace lux::authoring
{
    bool readMaterialGraph(
        const lux::asset::LuxAsset& asset,
        lux::rdesc::MaterialGraph&  graph,
        std::string*                error
    ) noexcept
    {
        const auto* bytes = asset.payload(kMaterialGraphPayloadTag);
        if (bytes == nullptr)
        {
            if (error != nullptr)
                *error = "material has no authoring graph payload";
            return false;
        }
        return detail::decodeMaterialGraph(
            std::span<const std::byte>{bytes->data(), bytes->size()},
            graph,
            error
        );
    }

    void attachMaterialGraph(
        lux::asset::LuxAsset&            asset,
        const lux::rdesc::MaterialGraph& graph
    )
    {
        asset.setPayload(
            kMaterialGraphPayloadTag,
            detail::encodeMaterialGraph(graph)
        );
    }
} // namespace lux::authoring
