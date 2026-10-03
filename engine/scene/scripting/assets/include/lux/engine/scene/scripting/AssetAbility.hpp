#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>
#include <lux/engine/scene/scripting/ScriptAssetResult.hpp>

namespace lux::scene::script
{
    struct LUX_SCRIPT_ABILITY(
        id = lux.scene.assets, name = Assets, display = Assets, version = 1, receiver = provider_instance
    ) AssetAbility
    {
        LUX_SCRIPT_QUERY(id = lux.scene.assets.assetId, display = assetId, result_lifetime = owned_value)
        lux::asset::AssetId assetId(
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::uint32_t first,
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::uint32_t second,
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::uint32_t third,
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::uint32_t fourth
        ) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.assets.assetWord, display = assetWord, result_lifetime = owned_value)
        std::uint32_t assetWord(
            LUX_SCRIPT_PARAM(lifetime = owned_value) lux::asset::AssetId id,
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::uint32_t word
        ) noexcept;

        LUX_SCRIPT_ASYNC(id = lux.scene.assets.readAsset, display = readAsset, result_lifetime = awaitable)
        ScriptAssetReadOutcome readAsset(LUX_SCRIPT_PARAM(lifetime = owned_value) lux::asset::AssetId id) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.assets.succeeded, display = succeeded, result_lifetime = owned_value)
        bool succeeded(LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetReadOutcome result) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.assets.handle, display = handle, result_lifetime = owned_value)
        ScriptAssetHandle handle(LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetReadOutcome result) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.assets.errorDomain, display = errorDomain, result_lifetime = owned_value)
        std::uint32_t errorDomain(LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetReadOutcome result) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.assets.errorCode, display = errorCode, result_lifetime = owned_value)
        std::uint32_t errorCode(LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetReadOutcome result) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.assets.describeAsset, display = describeAsset, result_lifetime = owned_value)
        ScriptAssetInspection describeAsset(LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetHandle handle) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.assets.inspectionError, display = inspectionError, result_lifetime = owned_value)
        std::uint32_t inspectionError(LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetInspection result) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.assets.inspectedId, display = inspectedId, result_lifetime = owned_value)
        lux::asset::AssetId inspectedId(LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetInspection result) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.assets.imageSizeWord, display = imageSizeWord, result_lifetime = owned_value)
        std::uint32_t imageSizeWord(
            LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetInspection result,
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::uint32_t word
        ) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.assets.copyAssetBytes, display = copyAssetBytes, result_lifetime = owned_value)
        ScriptAssetBytes copyAssetBytes(
            LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetHandle handle,
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::uint32_t offset_high,
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::uint32_t offset_low,
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::uint32_t count
        ) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.assets.bytesError, display = bytesError, result_lifetime = owned_value)
        std::uint32_t bytesError(LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetBytes result) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.assets.bytesCount, display = bytesCount, result_lifetime = owned_value)
        std::uint32_t bytesCount(LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetBytes result) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.assets.byteAt, display = byteAt, result_lifetime = owned_value)
        std::int32_t byteAt(
            LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetBytes result,
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::uint32_t index
        ) noexcept;

        LUX_SCRIPT_COMMAND(id = lux.scene.assets.releaseAsset, display = releaseAsset, result_lifetime = owned_value)
        void releaseAsset(LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetHandle handle) noexcept;

    };
}
