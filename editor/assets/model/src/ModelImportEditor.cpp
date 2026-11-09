#include <lux/engine/editor/ModelImportEditor.hpp>
#include <lux/engine/editor/ModelImportValues.value_editor.hpp>
#include <lux/engine/editor/ModelImporter.hpp>

namespace lux::editor
{
    AssetImportEditor modelImportEditor() noexcept
    {
        return declareAssetImportEditor<ModelImporter>();
    }
} // namespace lux::editor
