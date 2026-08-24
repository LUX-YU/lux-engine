#pragma once
#include <lux/engine/description/Model.hpp>
#include <lux/engine/description/ImportedMaterialDesc.hpp>   // W5b transient import descs
#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace lux::asset
{
	class LUX_ASSET_PUBLIC ModelAsset : public TAsset<lux::rdesc::ModelNode>
	{
	public:
		static constexpr EAssetType asset_type{ EAssetType::MODEL };

		explicit ModelAsset(std::unique_ptr<AssetInfo> info);

		~ModelAsset() override;

		/// Mesh asset IDs referenced by ModelMeshInfo::mesh_index
		[[nodiscard]] const std::vector<asset_id_t>& meshAssetIds() const noexcept { return mesh_asset_ids_; }
		void addMeshAssetId(const asset_id_t& id) { mesh_asset_ids_.push_back(id); }

		/// Material asset IDs referenced by ModelMeshInfo::material_index
		[[nodiscard]] const std::vector<asset_id_t>& materialAssetIds() const noexcept { return material_asset_ids_; }
		void addMaterialAssetId(const asset_id_t& id) { material_asset_ids_.push_back(id); }

		/// Re-point material slot @p index to another material asset, in place.
		/// Strictly POSITIONAL (same length/order) so ModelMeshInfo::material_index
		/// ordinals stay valid — used by import-time auto-conversion to swap a builtin
		/// material id for its baked graph-material id. No-op if @p index is OOB.
		void setMaterialAssetId(std::size_t index, const asset_id_t& id)
		{
			if (index < material_asset_ids_.size())
				material_asset_ids_[index] = id;
		}

		/// Optional skeleton asset extracted from the model (e.g. for skeletal meshes
		/// imported from .fbx / .gltf with rigging). Empty when the source had no bones.
		[[nodiscard]] const std::optional<asset_id_t>& skeletonAssetId() const noexcept
		{ return skeleton_asset_id_; }
		void setSkeletonAssetId(const asset_id_t& id) { skeleton_asset_id_ = id; }

		/// AnimationClip asset IDs extracted from the source's aiAnimation entries.
		/// Each clip targets the skeleton at skeletonAssetId() by bone-index lookup.
		[[nodiscard]] const std::vector<asset_id_t>& animationClipAssetIds() const noexcept
		{ return animation_clip_asset_ids_; }
		void addAnimationClipAssetId(const asset_id_t& id) { animation_clip_asset_ids_.push_back(id); }

		/// A cooked model is runtime-ready once its dependency manifest has
		/// been decoded, even when the optional authoring node tree is absent.
		void markManifestLoaded() noexcept { manifest_loaded_ = true; }
		[[nodiscard]] bool hasData() const override
		{
			return manifest_loaded_ || TAsset::hasData();
		}

		// ===== W5b: transient imported-material descriptors (NOT persisted) =====
		// The Assimp importer fills these (one per material slot, parallel to
		// ModelMeshInfo::material_index); the editor's AssetImporter converts each to a
		// graph + bakes a MaterialAsset, then fills materialAssetIds() with the
		// baked uuids. At runtime (loading a baked .luxmodel) these stay empty —
		// materialAssetIds() carries the graph-material uuids read from the manifest.
		[[nodiscard]] const std::vector<rdesc::ImportedMaterialDesc>& importedMaterialDescs() const noexcept
		{ return imported_material_descs_; }
		/// Per-desc texture-asset uuid list (slot i == ImportedTextureRef::texture_index).
		[[nodiscard]] const std::vector<std::vector<asset_id_t>>& importedMaterialTextureUuids() const noexcept
		{ return imported_material_texture_uuids_; }
		void addImportedMaterialDesc(rdesc::ImportedMaterialDesc desc, std::vector<asset_id_t> tex_uuids)
		{
			imported_material_descs_.push_back(std::move(desc));
			imported_material_texture_uuids_.push_back(std::move(tex_uuids));
		}

	private:
		std::vector<asset_id_t>   mesh_asset_ids_;
		std::vector<asset_id_t>   material_asset_ids_;
		std::optional<asset_id_t> skeleton_asset_id_;
		std::vector<asset_id_t>   animation_clip_asset_ids_;
		bool                      manifest_loaded_{false};
		// W5b transient (import-only):
		std::vector<rdesc::ImportedMaterialDesc>   imported_material_descs_;
		std::vector<std::vector<asset_id_t>>       imported_material_texture_uuids_;
	};
}
