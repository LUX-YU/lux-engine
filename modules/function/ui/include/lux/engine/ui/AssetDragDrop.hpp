#pragma once
/**
 * @file AssetDragDrop.hpp
 * @brief Wire-format payload exchanged through ImGui DragDrop between the
 *        AssetBrowser (source) and viewport / Inspector targets.
 *
 * Lives in `function::ui` rather than in `engine/editor` for layering: the
 * SceneViewportPanel here in ui needs to declare the drop target type, and
 * `engine/editor` is a tier above us so we cannot pull its headers. The
 * payload is intentionally a raw-bytes POD so this header carries zero
 * dependency on the asset module (which the ui module is forbidden from
 * importing — see modules/function/ui/CMakeLists.txt).
 *
 * Encoding contract (matched by both sides):
 *   - `uuid_bytes` is the underlying `lux::asset::asset_id_t` bytes (16B,
 *     network / big-endian field order per stduuid's `as_bytes()`).
 *   - `asset_type` is the raw value of `lux::asset::EAssetType` cast to
 *     u8 (the enum is small; values are stable as long as `Asset.hpp`
 *     doesn't reorder entries).
 *   - `is_model` is 1 when the source file has the `.luxmodel` extension
 *     (the `MODEL` magic), 0 otherwise. The viewport spawn path branches
 *     on this without having to consult AssetManager again.
 *   - `display_name` is a zero-padded UTF-8 string for the drag-ghost
 *     label ImGui draws under the cursor. Truncated silently if the
 *     source name exceeds the buffer.
 */

#include <cstdint>
#include <type_traits>

namespace lux::ui
{
    /// ImGui DragDrop type tag. ImGui silently truncates to 32 chars, so
    /// keep it short and stable; both sides hash by exact string match.
    inline constexpr const char* kAssetDragPayloadTag = "LUX_ASSET";

    /// POD payload memcpy'd through `ImGui::SetDragDropPayload`. Must stay
    /// trivially copyable — ImGui copies it by raw bytes.
    struct AssetDragPayload
    {
        std::uint8_t uuid_bytes[16]{};
        std::uint8_t asset_type{};
        std::uint8_t is_model{};
        std::uint8_t _pad[14]{};       ///< reserve room for future flags / type fields
        char         display_name[64]{};
    };
    static_assert(std::is_trivially_copyable_v<AssetDragPayload>,
                  "AssetDragPayload must be trivially copyable for ImGui DragDrop");
    static_assert(sizeof(AssetDragPayload) == 96,
                  "AssetDragPayload size locked at v1 — bump version & tag on changes");

} // namespace lux::ui
