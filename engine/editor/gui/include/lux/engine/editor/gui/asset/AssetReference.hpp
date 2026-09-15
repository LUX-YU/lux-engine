#pragma once

#include <cstddef>
#include <cstring>
#include <lux/engine/editor/DocumentRequests.hpp>
#include <lux/engine/editor/gui/visibility.h>
#include <lux/engine/editor/project/AssetCatalog.hpp>
#include <span>
#include <type_traits>

namespace lux::editor
{
class Project;
}

namespace lux::editor::gui
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

// Draw in the current UI frame/ID scope. Rejected references retain the input value.
[[nodiscard]] LUX_EDITOR_GUI_PUBLIC EditorResult<bool> drawAssetReference(const Project &, asset::AssetId &,
                                                                          std::uint32_t required_magic);
} // namespace lux::editor::gui
