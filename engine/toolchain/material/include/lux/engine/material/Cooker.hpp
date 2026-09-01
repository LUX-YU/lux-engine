#pragma once

#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/material/ImportedMaterialDescription.hpp>
#include <lux/engine/material/cooker/visibility.h>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace lux::material
{
    enum class EMaterialCookError : std::uint8_t
    {
        INVALID_ASSET_INFO,
        IMPORT_FAILURE,
        COMPILE_FAILURE,
        INVALID_ASSET,
        ALLOCATION_FAILURE
    };

    struct MaterialCookFailure final
    {
        EMaterialCookError code{EMaterialCookError::INVALID_ASSET_INFO};
        std::string message;
        std::optional<MaterialCompileFailure> compile_failure;
    };

    [[nodiscard]] LUX_ENGINE_MATERIAL_COOKER_PUBLIC lux::cxx::expected<
        std::shared_ptr<const lux::asset::MaterialAsset>,
        MaterialCookFailure
    > cookMaterial(lux::asset::AssetInfo info, const MaterialGraph& graph) noexcept;

    [[nodiscard]] LUX_ENGINE_MATERIAL_COOKER_PUBLIC lux::cxx::expected<
        std::shared_ptr<const lux::asset::MaterialAsset>,
        MaterialCookFailure
    > cookImportedMaterial(
        lux::asset::AssetInfo info,
        const ImportedMaterialDescription& imported
    ) noexcept;
} // namespace lux::material
