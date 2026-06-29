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

#include <lux/engine/asset/Asset.hpp>           // asset_id_t
#include <lux/engine/editor/visibility.h>

namespace lux::asset { class AssetManager; }

namespace lux::editor
{
    /// Stable UUID strings for the editor's built-in mesh / material
    /// assets. Picked from the v4 reserved-bit space with a distinctive
    /// trailing hex pattern to make collisions with user-generated UUIDs
    /// astronomically unlikely.
    inline constexpr const char* kBuiltinCubeMeshIdStr        = "00000000-0000-4000-8000-cccccccccccc";
    inline constexpr const char* kBuiltinPlaneMeshIdStr       = "00000000-0000-4000-8000-ffffffffffff";
    inline constexpr const char* kBuiltinSphereMeshIdStr      = "00000000-0000-4000-8000-eeeeeeeeeeee";
    inline constexpr const char* kBuiltinWhitePbrMaterialIdStr = "00000000-0000-4000-8000-aaaaaaaaaaaa";

    /// Built-in EMISSIVE materials — a small colour palette of glowing "bulb"
    /// materials (PBR + bright Emissive). Each demo light pairs a same-coloured
    /// bulb mesh so the light SOURCE is visible, not just its lit effect. The
    /// UUIDs are stable so a generated scene resolves them on load; both the
    /// runtime baker (EditorBuiltins) and the scene generator (DemoSceneTemplate)
    /// read this one table so colours + ids stay in lock-step.
    inline constexpr int kBuiltinEmissiveCount = 8;
    inline constexpr const char* kBuiltinEmissiveIdStrs[kBuiltinEmissiveCount] = {
        "00000000-0000-4000-8001-000000000001",
        "00000000-0000-4000-8001-000000000002",
        "00000000-0000-4000-8001-000000000003",
        "00000000-0000-4000-8001-000000000004",
        "00000000-0000-4000-8001-000000000005",
        "00000000-0000-4000-8001-000000000006",
        "00000000-0000-4000-8001-000000000007",
        "00000000-0000-4000-8001-000000000008",
    };
    /// Linear-RGB emission per palette slot (kept bright + distinct).
    inline constexpr float kBuiltinEmissiveColors[kBuiltinEmissiveCount][3] = {
        {1.00f, 0.90f, 0.70f},   // 0 warm white
        {1.00f, 0.15f, 0.10f},   // 1 red
        {0.20f, 1.00f, 0.25f},   // 2 green
        {0.20f, 0.40f, 1.00f},   // 3 blue
        {1.00f, 0.55f, 0.10f},   // 4 amber
        {0.10f, 0.90f, 1.00f},   // 5 cyan
        {1.00f, 0.20f, 0.80f},   // 6 magenta
        {0.60f, 0.30f, 1.00f},   // 7 violet
    };

    class LUX_EDITOR_PUBLIC EditorBuiltins
    {
    public:
        EditorBuiltins() = default;

        EditorBuiltins(const EditorBuiltins&)            = delete;
        EditorBuiltins& operator=(const EditorBuiltins&) = delete;

        /// Build + register the built-in assets into @p mgr. Returns
        /// `false` only on a programmer error (UUID literal failed to
        /// parse, asset register collision). A missing skybox texture file
        /// is NOT a failure — the function returns `true` with
        /// `skyboxTextureId().is_nil() == true`.
        [[nodiscard]] bool registerInto(lux::asset::AssetManager& mgr);

        lux::asset::asset_id_t cubeMeshId()         const noexcept { return cube_mesh_id_; }
        lux::asset::asset_id_t planeMeshId()        const noexcept { return plane_mesh_id_; }
        lux::asset::asset_id_t sphereMeshId()       const noexcept { return sphere_mesh_id_; }
        lux::asset::asset_id_t whitePbrMaterialId() const noexcept { return white_pbr_id_; }
        lux::asset::asset_id_t skyboxTextureId()    const noexcept { return skybox_tex_id_; }

        [[nodiscard]] bool isReady() const noexcept { return ready_; }

    private:
        lux::asset::asset_id_t cube_mesh_id_{};
        lux::asset::asset_id_t plane_mesh_id_{};
        lux::asset::asset_id_t sphere_mesh_id_{};
        lux::asset::asset_id_t white_pbr_id_{};
        lux::asset::asset_id_t skybox_tex_id_{};
        bool                    ready_{false};
    };

} // namespace lux::editor
