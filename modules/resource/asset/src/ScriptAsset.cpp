#include <lux/engine/asset/ScriptAsset.hpp>

namespace lux::asset
{
    ScriptAsset::ScriptAsset(std::unique_ptr<AssetInfo> info,
                             std::unique_ptr<Script>    script,
                             std::vector<std::byte>     payload)
        : TAsset<Script>(std::move(info), std::move(script))
        , payload_(std::move(payload))
    {
    }
}
