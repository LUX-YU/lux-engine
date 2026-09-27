#pragma once

#include <cstddef>
#include <cstring>
#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/ui/visibility.h>
#include <lux/engine/editor/storage/AssetCatalog.hpp>
#include <span>
#include <type_traits>

namespace lux::editor
{
    class ProjectStorage;
}

namespace lux::editor::ui
{
    inline constexpr char kAssetReferencePayload[] = "lux.editor.asset-reference.v2";
    static_assert(std::is_trivially_copyable_v<AssetReference>);

    [[nodiscard]] inline EditorResult<AssetReference> decodeAssetReference(std::span<const std::byte> bytes)
    {
        if (bytes.size() != sizeof(AssetReference))
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "ui.asset-payload"});
        }

        AssetReference reference;
        std::memcpy(&reference, bytes.data(), sizeof(reference));
        if (!reference.project_instance || !reference.catalog_revision || reference.asset.isNull())
        {
            return lux::cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "ui.asset-payload"});
        }

        return reference;
    }

} // namespace lux::editor::ui
