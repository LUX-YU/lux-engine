#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/assets/visibility.h>
#include <lux/engine/toolchain/asset/model/ModelCooker.hpp>
#include <span>

namespace lux::editor::assets
{
    struct ModelImportFile final
    {
        std::string path;
        std::string digest;
    };
    struct ModelImportRecipe final
    {
        std::string root;
        std::string entry;
        toolchain::ModelCookConfiguration configuration;
        std::vector<ModelImportFile> files;
    };

    // Pure recipe codec. Reading referenced files and cooking remain separate activities.
    [[nodiscard]] LUX_EDITOR_ASSETS_PUBLIC EditorResult<ModelImportRecipe>
    decodeModelImportRecipe(std::span<const std::byte>);
    [[nodiscard]] LUX_EDITOR_ASSETS_PUBLIC EditorResult<std::vector<std::byte>>
    encodeModelImportRecipe(const ModelImportRecipe&);
}
