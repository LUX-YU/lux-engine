#include <lux/engine/editor/material/MaterialPreview.hpp>
void changeRecipe(lux::editor::material::MaterialPreview& preview,
    lux::editor::material::MaterialCompileInputKey input, lux::asset::AssetId mesh, lux::cxx::SharedBytes<> bytes)
{
    (void)preview.setDesired(input, lux::editor::material::MaterialPreviewRecipe{mesh, bytes});
}
