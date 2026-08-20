#include <lux/engine/resource/asset/ScriptAsset.hpp>

namespace lux::asset
{
    ScriptAsset::ScriptAsset(std::unique_ptr<AssetInfo> info,
                             std::unique_ptr<lux::rdesc::Script> script,
                             std::vector<std::byte>     payload)
        : TAsset<lux::rdesc::Script>(std::move(info), std::move(script))
        , payload_(std::move(payload))
    {
    }
}
