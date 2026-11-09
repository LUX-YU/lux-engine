#pragma once

#include <entt/signal/sigh.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/assets/visibility.h>

namespace lux::asset
{
    class Asset;
}

namespace lux::engine::assets::detail
{
    struct AssetRecord;
    struct AssetDataRecord;

    // References borrow stable repository records. These functions never allocate or query a directory.
    LUX_ENGINE_ASSETS_PUBLIC void retainAsset(AssetRecord&) noexcept;
    LUX_ENGINE_ASSETS_PUBLIC void releaseAsset(AssetRecord&) noexcept;
    [[nodiscard]] LUX_ENGINE_ASSETS_PUBLIC asset::AssetId assetIdentity(const AssetRecord&) noexcept;
    [[nodiscard]] LUX_ENGINE_ASSETS_PUBLIC entt::sigh<void(asset::AssetId)>&
    assetContentChanged(AssetRecord&) noexcept;
    LUX_ENGINE_ASSETS_PUBLIC void retainData(AssetDataRecord&) noexcept;
    LUX_ENGINE_ASSETS_PUBLIC void releaseData(AssetDataRecord&) noexcept;
    [[nodiscard]] LUX_ENGINE_ASSETS_PUBLIC AssetRecord& assetRecord(AssetDataRecord&) noexcept;
    [[nodiscard]] LUX_ENGINE_ASSETS_PUBLIC const void* dataAddress(const AssetDataRecord&) noexcept;
    [[nodiscard]] LUX_ENGINE_ASSETS_PUBLIC cxx::type_info dataType(const AssetDataRecord&) noexcept;
    [[nodiscard]] LUX_ENGINE_ASSETS_PUBLIC const asset::Asset* originalAsset(const AssetDataRecord&) noexcept;
}
