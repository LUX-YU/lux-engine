#pragma once

#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/toolchain/asset/material/ImportedMaterialDescription.hpp>
#include <lux/engine/toolchain/asset/material/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace lux::material
{
    class MaterialGraph;
}

namespace lux::toolchain
{
    enum class EMaterialCookError : std::uint8_t
    {
        INVALID_INPUT,
        LOWERING_FAILED,
        SHADER_COMPILE_FAILED,
        INVALID_ASSET,
        ALLOCATION_FAILURE,
    };

    struct MaterialCookFailure final
    {
        EMaterialCookError code{EMaterialCookError::INVALID_INPUT};
        std::string detail;
    };

    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_MATERIAL_PUBLIC lux::cxx::expected<
        std::shared_ptr<const lux::asset::MaterialAsset>,
        MaterialCookFailure
    > cookMaterial(
        lux::asset::AssetInfo info,
        const lux::material::MaterialGraph& graph
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_MATERIAL_PUBLIC lux::cxx::expected<
        std::shared_ptr<const lux::asset::MaterialAsset>,
        MaterialCookFailure
    > cookImportedMaterial(
        lux::asset::AssetInfo info,
        const ImportedMaterialDescription& imported
    ) noexcept;
} // namespace lux::toolchain
