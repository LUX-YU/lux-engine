#include <lux/engine/process/asset_loading/AssetLoadSender.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>
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

    auto created = lux::process::ExecutionRuntime::create({
        1U,
        2U,
        2U,
        {2U},
        lux::process::BlockingSchedulerConfig{1U, 2U}
    });
    if (!created)
        return 1;
    auto runtime = std::move(*created);
    auto blocking = runtime.blocking();
    lux::asset::AssetVfs vfs;
    auto endpoint = lux::process::asset_loading::VfsAssetReadEndpoint::create(
        vfs.view(),
        *blocking,
        {2U}
    );
    if (!endpoint)
        return 2;
    auto port = (*endpoint)->port();
    (*endpoint)->requestStop();
    if (!(*endpoint)->join())
        return 3;
    runtime.requestStop();
    return runtime.join() && port ? 0 : 4;
}
