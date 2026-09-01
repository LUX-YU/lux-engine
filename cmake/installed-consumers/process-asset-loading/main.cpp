#include <lux/engine/process/asset_loading/AssetLoadSender.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>

#include <stdexec/execution.hpp>

int main()
{
    auto sender = lux::process::asset_loading::loadAsset<lux::asset::TextureAsset>(
        {},
        {},
        lux::asset::AssetDecodeLimits{1U, 1U, 0U}
    );
    static_assert(stdexec::sender<decltype(sender)>);
    return 0;
}
