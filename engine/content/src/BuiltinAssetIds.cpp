#include <lux/engine/content/BuiltinAssetIds.hpp>

namespace lux::engine::content
{
    const lux::asset::asset_id_t& builtinMissingMaterialId()
    {
        static const auto id = lux::asset::asset_id_t::from_string(
            kBuiltinMissingMaterialIdStr
        ).value();
        return id;
    }
} // namespace lux::engine::content
