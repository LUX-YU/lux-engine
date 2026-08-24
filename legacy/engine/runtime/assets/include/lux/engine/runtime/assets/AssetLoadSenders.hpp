#pragma once

#include <lux/engine/runtime/assets/AssetLoadService.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>

namespace lux::asset_runtime
{
    [[nodiscard]] inline auto
    loadAsset(const AssetClient& client, const lux::asset::asset_id_t& id) noexcept
    {
        return lux::exec::execute(client.loadClient(), client.loadOperation(id));
    }
}
