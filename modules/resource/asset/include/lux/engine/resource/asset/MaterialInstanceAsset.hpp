#pragma once
// ============================================================================
//  MaterialInstanceAsset.hpp — a UE-style Material INSTANCE.
//
//  An instance references a PARENT graph material (EAssetType::MATERIAL)
//  and OVERRIDES some of its parameters / texture slots — WITHOUT carrying its
//  own shader. So N instances of one Material share ONE PSO (the parent's baked
//  SPIR-V, deduped) and each gets its own per-instance SSBO param lane. Storing
//  any SPIR-V here would re-introduce the shader explosion, so this payload holds
//  ONLY overrides.
//
//  Overrides are keyed by LANE INDEX (mask bit i ⇒ overrides the parent's lane i),
//  matching the parent MaterialData::params[] / texture_slot_ids[] order —
//  because the baked parent persists param VALUES + count, not names (names live
//  only in the parent's graph). Name-keyed overrides (survive a param
//  reorder) are a follow-up that first needs param names persisted on the parent.
// ============================================================================

#include "Asset.hpp"               // TAsset, asset_id_t, EAssetType
#include "MaterialAsset.hpp"  // MaterialData::kMaxParams / kMaxTextures

#include <array>
#include <cstdint>

namespace lux::asset
{
    /// CPU-side payload of a material-instance asset.
    struct MaterialInstanceData
    {
        // Reuse the parent's lane capacities so the bridge merges lane-for-lane.
        static constexpr std::uint32_t kMaxParams   = MaterialData::kMaxParams;   // 16
        static constexpr std::uint32_t kMaxTextures = MaterialData::kMaxTextures;  // 8

        /// The parent graph material whose shader/PSO this instance shares.
        asset_id_t parent_material_id{};

        // ---- parameter overrides (sparse, by lane index) ----
        std::uint32_t param_override_mask{ 0 };          ///< bit i set ⇒ params[i] overrides parent lane i
        float         params[kMaxParams][4]{};

        // ---- texture-slot overrides (sparse, by slot index) ----
        std::uint32_t tex_override_mask{ 0 };            ///< bit i set ⇒ texture_slot_ids[i] overrides parent slot i
        std::array<asset_id_t, kMaxTextures> texture_slot_ids{};  ///< nil + bit set = explicitly cleared slot

        // ---- render-state override (optional; overriding it gives the instance its
        //      own PSO, so default is render_state_override==0 = inherit parent) ----
        std::uint32_t render_state_override{ 0 };        ///< 0 = inherit parent's render-state
        std::uint32_t alpha_mode{ 0 };                   ///< rdesc::EAlphaMode value (only if override != 0)
        bool          double_sided{ false };
        // (alpha_cutoff is NOT overridable — it is a baked SPIR-V literal on the parent.)
    };

    /// A material-instance asset.
    class LUX_ASSET_PUBLIC MaterialInstanceAsset : public TAsset<MaterialInstanceData>
    {
    public:
        static constexpr EAssetType asset_type{ EAssetType::MATERIAL_INSTANCE };
        explicit MaterialInstanceAsset(
            std::unique_ptr<AssetInfo> info,
            std::unique_ptr<MaterialInstanceData> data = nullptr
        );
        ~MaterialInstanceAsset() override;
    };

} // namespace lux::asset
