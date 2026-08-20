#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <cstdint>
#include <Eigen/Geometry>
#include <lux/engine/resource/asset/ModelAsset.hpp>
#include <lux/engine/toolchain/asset/texture/TextureImporter.hpp>
#include <lux/engine/resource/asset/codecs/MeshSerDeser.hpp>
#include <lux/engine/resource/asset/codecs/SkeletonSerDeser.hpp>
#include <lux/engine/resource/asset/codecs/AnimationClipSerDeser.hpp>
#include <lux/engine/description/ImportedMaterialDesc.hpp>   // W5b: importer emits POD descs (no rdesc::Material)

struct aiScene;
struct aiNode;
struct aiMesh;
struct aiMaterial;
struct aiTexture;
struct aiAnimation;

namespace lux::toolchain
{
	using lux::asset::AnimationClipSerDeser;
	using lux::asset::AssetIDPair;
	using lux::asset::AssetManager;
	using lux::asset::EAssetError;
	using lux::asset::LuxAsset;
	using lux::asset::MeshLoadConfig;
	using lux::asset::MeshSerDeser;
	using lux::rdesc::ModelMeshInfo;
	using lux::rdesc::ModelNode;
	using lux::asset::SkeletonSerDeser;
	using lux::asset::TAssetSerDeser;
	using lux::rdesc::Vertex;
	using lux::asset::asset_id_t;

	/**
	 * @brief Configuration structure for model serialization/deserialization.
	 * 
	 * This structure contains configuration parameters that control how models
	 * are loaded and processed, including settings for textures, meshes, materials,
	 * and coordinate system handling.
	 */
	struct ModelImportConfig
	{
		lux::toolchain::TextureImportConfig texture_load_config;
		MeshLoadConfig		mesh_load_config;      ///< Configuration for mesh loading
		// (material_load_config removed in W5b — the importer emits ImportedMaterialDesc
		//  PODs + the editor bakes graph materials; the old target_technique re-convert
		//  on the closure model is gone.)

		bool				is_right_handed{ true }; ///< Whether to use right-handed coordinate system

		// ── Import Options: axis/scale correction baked into the asset ──────
		/// Pre-rotation baked into the model at import (e.g. -90° about X to turn
		/// a glTF Z-up character upright in this Y-up engine). Applied as an
		/// "armature prefix" for skinned meshes (Skeleton::global_transform) and
		/// baked into vertices for static meshes — so spawn needs no fix-up.
		Eigen::Quaternionf	pre_rotate{ Eigen::Quaternionf::Identity() };
		/// Uniform scale baked alongside pre_rotate (e.g. 0.01 for cm→m).
		float				uniform_scale{ 1.0f };
		/// When false, aiAnimation entries are skipped (no AnimationClip assets).
		bool				import_animations{ true };

		/// Base seed for deterministic (name-based) sub-asset ids. When empty
		/// (the default) every sub-asset gets a random id, preserving legacy
		/// behavior. The editor's AssetImporter sets this to a content-relative
		/// identity (e.g. "Models/CesiumMan") so re-importing the same source
		/// reproduces identical ids and keeps the on-disk reference graph intact.
		std::string			deterministic_seed;
	};

	/**
	 * @brief Forward declaration of LoadContext structure.
	 */
	struct LoadContext;

	/**
	 * @brief Serializer/deserializer for 3D model assets.
	 * 
	 * The ModelImporter class handles importing 3D models from external file formats
	 * (such as glTF, OBJ, FBX) using the Assimp library. It processes the complete
	 * model hierarchy including meshes, materials, and textures, converting them
	 * into engine-specific asset representations.
	 */
	// Authoring importer implementation. It is compiled into the Toolchain
	// target, not exported by the Runtime asset DLL.
	class ModelImporter final : public TAssetSerDeser<ModelImportConfig>
	{
	public:
		/**
		 * @brief Constructs a ModelImporter with the specified asset manager.
		 * @param manager Shared pointer to the asset manager
		 */
		explicit ModelImporter(std::shared_ptr<AssetManager> manager);

		/**
		 * @brief Virtual destructor for ModelImporter.
		 */
		~ModelImporter() override;

		/**
		 * @brief Imports a model from an external file.
		 * @param external_path Path to the external model file
		 * @return Expected result containing asset pointer and ID pair, or error code
		 */
		[[nodiscard]] lux::cxx::expected<AssetIDPair, EAssetError>
		importFromFile(const std::filesystem::path& external_path) override;

	private:
		/**
	     * @brief Deserializes a model from a Lux asset file.
	     * @param path Input file stream containing the serialized asset
	     * @return Expected result containing unique asset pointer, or error code
	     */
		lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
		fromLuxAssetStream(std::istream& path) override;

		/**
 		 * @brief Serializes a model asset to a Lux asset file.
 		 * @param asset The asset to serialize
 		 * @param path Output file stream for the serialized data
 		 * @return Error code indicating success or failure
 		 */
		EAssetError
		exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& path) override;

		/**
		 * @brief Imports a texture asset from file or embedded data.
		 * @param ctx Loading context containing caches and scene information
		 * @param absPathOrEmbeddedKey Absolute file path or embedded texture key
		 * @param embeddedIfAny Pointer to embedded texture data if available
		 * @param type Texture type identifier
		 * @return Optional asset ID of the imported texture
		 */
		std::optional<asset_id_t> importTextureAsset(LoadContext& ctx,
			const std::filesystem::path& absPathOrEmbeddedKey,
			const aiTexture* embeddedIfAny, uint8_t type
		);

		/**
		 * @brief Creates a texture binding from an aiMaterial texture stack.
		 * @param ctx Loading context containing caches and scene information
		 * @param mat The aiMaterial to extract texture information from
		 * @param type The texture type to query
		 * @param index The texture index within the type stack
		 * @param scene The aiScene containing embedded texture data
		 * @return Optional TextureBinding if texture exists and is successfully processed
		 */
		std::optional<rdesc::ImportedTextureRef> makeBindingFor(
			LoadContext& ctx, const aiMaterial* mat,
			uint8_t type, unsigned index, const aiScene* scene,
			std::vector<asset_id_t>& texture_ids
		);

		/**
		 * @brief Imports an aiMaterial into an ImportedMaterialDesc (W5b — no closure
		 *        rdesc::Material / MaterialAsset). Appends the desc + its texture-UUID
		 *        list to the load context.
		 * @return The desc's ORDINAL (== ModelMeshInfo::material_index).
		 */
		std::uint32_t importMaterial(LoadContext& ctx, unsigned matIdx);

		/**
		 * @brief Appends a default (neutral) ImportedMaterialDesc for meshes with no
		 *        source material. @return its ordinal.
		 */
		std::uint32_t ensureDefaultUnlit(LoadContext& ctx);

		/**
		 * @brief Fills an ImportedMaterialDesc from a Phong/Blinn-Phong aiMaterial
		 *        (legacy branch — sets is_legacy so the graph collapses to PBR).
		 */
		void fillLegacyLit(rdesc::ImportedMaterialDesc& out, const aiMaterial* m,
			LoadContext& ctx, const aiScene* scene,
			std::vector<asset_id_t>& texture_ids
		);

		/**
		 * @brief Fills an ImportedMaterialDesc from a PBR metallic-roughness aiMaterial.
		 */
		void fillPBR(rdesc::ImportedMaterialDesc& out, const aiMaterial* m,
			LoadContext& ctx, const aiScene* scene,
			std::vector<asset_id_t>& texture_ids
		);

		/**
		 * @brief Loads model data from a file path.
		 * @param path Path to the model file
		 * @param model_data Model node to populate with loaded data
		 * @return Error code indicating success or failure
		 */
		EAssetError loadFrom(const std::filesystem::path& path, ModelNode& model_data,
			std::vector<asset_id_t>&                  mesh_ids_out,
			std::vector<rdesc::ImportedMaterialDesc>& material_descs_out,
			std::vector<std::vector<asset_id_t>>&     material_desc_tex_out,
			std::optional<asset_id_t>&                skeleton_id_out,
			std::vector<asset_id_t>&                  animation_clip_ids_out);

		/**
		 * @brief Extract skeleton hierarchy from the loaded aiScene.
		 *
		 * Walks every aiMesh's aiBone[] to collect bone names, then finds each bone's
		 * position in the aiNode tree to determine the parent_index chain. Bones are
		 * topologically sorted so parent_index < self_index for every bone (the
		 * Skeleton runtime invariant).
		 *
		 * Output:
		 *   - One SkeletonAsset registered into the AssetManager. Its id is set on
		 *     `ctx.skeleton_asset_id`.
		 *   - `ctx.bone_name_to_index` populated for use by processMesh's vertex-
		 *     influence filling.
		 *
		 * If the scene contains no aiBone references (a static mesh), this is a no-op
		 * and ctx.skeleton_asset_id remains empty.
		 */
		void buildSkeleton(LoadContext& context);

		/**
		 * @brief Fill per-vertex bone influences for one aiMesh.
		 *
		 * For every aiBone of the mesh, iterate its aiVertexWeight list and accumulate
		 * (bone_index, weight) pairs per affected vertex. After collection, sort by
		 * weight desc, keep top-4 (max_bone_influence), and renormalize so weights
		 * sum to 1.0 for vertices that received any influence.
		 *
		 * Vertices not affected by any bone keep bone_ids = {-1,-1,-1,-1} and
		 * weights = {0,0,0,0} — they pass through skinning untouched at runtime.
		 */
		void fillBoneInfluences(LoadContext& context,
		                        aiMesh* mesh,
		                        std::vector<Vertex>& verts);

		/**
		 * @brief Convert every aiAnimation in the scene into an AnimationClipAsset.
		 *
		 * Resolves each aiNodeAnim's mNodeName to a bone_index via the map populated
		 * by buildSkeleton(); channels not matching any bone are skipped with a
		 * warning. aiAnimation.mTicksPerSecond is multiplied into the key times so
		 * AnimationClip.duration is in seconds.
		 *
		 * Each resulting AnimationClipAsset is registered with the AssetManager and
		 * its id appended to ctx.animation_clip_asset_ids.
		 */
		void extractAnimations(LoadContext& context);
		
		/**
		 * @brief Processes an Assimp scene node recursively.
		 * @param context Loading context
		 * @param node The model node to populate
		 * @param assimp_node The Assimp node to process
		 */
		void processNode(LoadContext& context, ModelNode& node, const aiNode* assimp_node);
		
		/**
		 * @brief Processes an Assimp mesh and creates corresponding assets.
		 * @param context Loading context
		 * @param mesh_tex_map Mesh information structure to populate
		 * @param mesh The Assimp mesh to process
		 */
		void processMesh(LoadContext& context, ModelMeshInfo& mesh_tex_map, aiMesh* mesh);
		
		/**
		 * @brief Loads material textures for a mesh.
		 * @param context Loading context
		 * @param mesh_tex_map Mesh information structure to update
		 * @param mat The Assimp material containing textures
		 * @param type The texture type to load
		 */
		void loadMaterialTextures(LoadContext& context, ModelMeshInfo& mesh_tex_map, aiMaterial* mat, uint8_t type);

		/**
		 * @brief Builds a deterministic sub-asset seed from the configured base.
		 *
		 * Returns an empty string (→ random id) when no base seed is set in the
		 * config, so import remains opt-in. Otherwise composes
		 * "<base>|<kind>" or "<base>|<kind>|<index>".
		 *
		 * @param kind  Sub-asset category, e.g. "mesh", "material", "skeleton".
		 * @param index Optional ordinal disambiguating siblings of the same kind.
		 */
		std::string seedFor(std::string_view kind,
			std::optional<std::uint64_t> index = std::nullopt) const;

		MeshSerDeser 						 mesh_loader_;     ///< Mesh serialization/deserialization handler
		lux::toolchain::TextureImporter texture_loader_;
		// (material_loader_ removed in W5b — the importer emits ImportedMaterialDesc PODs;
		//  the editor bakes MaterialAssets, so ModelImporter writes no material asset.)
	};
} // namespace lux::toolchain
