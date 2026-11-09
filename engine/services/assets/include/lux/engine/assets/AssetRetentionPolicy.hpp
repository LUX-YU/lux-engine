#pragma once

#include <lux/engine/resource/identity/AssetId.hpp>
#include <span>
#include <lux/cxx/compile_time/type_info.hpp>
#include <vector>

namespace lux::engine::assets
{
    class AssetRetentionPolicy
    {
    public:
        virtual ~AssetRetentionPolicy() = default;

        // Only unused results are candidates. Empty purpose denotes CPU data; context distinguishes providers.
        virtual void selectReleases(
            cxx::type_info purpose,
            const void* context,
            std::span<const asset::AssetId> candidates,
            std::vector<asset::AssetId>& selected
        ) noexcept = 0;
    };
}
