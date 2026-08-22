#pragma once
/**
 * @file EditorBuiltins.hpp
 * @brief Editor-scoped "always available" assets (cube, plane, sphere,
 *        white PBR material, default skybox texture).
 *
 * Built once per editor process, owned by `LuxEditor`, lifetime survives
 * scene swaps. Asset UUIDs are stable compile-time constants so demo /
 * default entity wiring can refer to them deterministically — and so a
 * scene saved with a reference to e.g. `kBuiltinCubeMeshId` resolves on
 * reload without any string-name indirection. The skybox texture is the
 * exception: its UUID is whatever the on-disk `.luxasset` file carries,
 * captured at load time and exposed via `skyboxTextureId()`.
 *
 * If the on-disk skybox texture is missing (e.g. running outside the
 * build tree), `skyboxTextureId()` returns a nil UUID — scenes that
 * reference it then render no skybox, same fallback semantics as M1.
 */

#include <lux/engine/resource/asset/Asset.hpp>           // asset_id_t
#include <lux/engine/content/BuiltinAssetIds.hpp> // 引擎级冻结 id(M_Missing)
#include <lux/engine/editor/visibility.h>

namespace lux::asset { class AssetManager; }

namespace lux::editor
{
    // Stable built-in asset identities live in resource::asset's
    // BuiltinAssetIds.hpp. EditorBuiltins owns only editor-process adoption.

    class LUX_EDITOR_PUBLIC EditorBuiltins
    {
    public:
        EditorBuiltins() = default;

        EditorBuiltins(const EditorBuiltins&)            = delete;
        EditorBuiltins& operator=(const EditorBuiltins&) = delete;

        /// Build + register the built-in assets into @p mgr. Returns
        /// `false` only on a programmer error (UUID literal failed to
        /// parse, asset register collision). A missing skybox texture file
        /// is NOT a failure — the built-in skybox is simply absent then.
        ///
        /// 消费者一律用编译期 UUID 字符串常量(kBuiltinCubeMeshIdStr 等)
        /// 引用这些资产 —— 曾有一排逐 id 的访问器,全仓零调用方,已删。
        [[nodiscard]] bool registerInto(lux::asset::AssetManager& mgr);

    private:
        lux::asset::asset_id_t cube_mesh_id_{};
        lux::asset::asset_id_t plane_mesh_id_{};
        lux::asset::asset_id_t sphere_mesh_id_{};
        lux::asset::asset_id_t white_pbr_id_{};
        lux::asset::asset_id_t preview_grey_id_{};
        lux::asset::asset_id_t white_pbr_inst_id_{};
        lux::asset::asset_id_t skybox_tex_id_{};
        bool                    ready_{false};
    };

} // namespace lux::editor
