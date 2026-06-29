#include <lux/engine/asset/ModelSerDeser.hpp>
#include <lux/engine/asset/TextureSerDeser.hpp>
#include <lux/engine/asset/TextureAsset.hpp>
// (MaterialAsset.hpp + MaterialConverter.hpp dropped in W5b — the importer emits
//  ImportedMaterialDesc PODs; no closure rdesc::Material is produced.)
#include <lux/engine/asset/SkeletonAsset.hpp>
#include <lux/engine/asset/AnimationClipAsset.hpp>
#include <AssetManagerImpl.hpp>

#include <algorithm>
#include <unordered_map>
#include <charconv>
#include <filesystem>
#include <format>
#include <optional>
#include <string_view>
#include <iostream>
#include <cassert>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/anim.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/GltfMaterial.h>

#include <meshoptimizer.h>

namespace lux::asset
{
    // Description-layer enums the importer uses directly. These used to reach this
    // TU (unqualified) via MaterialAsset.hpp's `using lux::rdesc::…` re-exports; that
    // closure header is gone (W5c), so name them here. Both survive the closure
    // material deletion (ETextureColorSpace in Texture.hpp; EAlphaMode in
    // MaterialEnums.hpp), reachable via ImportedMaterialDesc.hpp.
    using rdesc::EAlphaMode;
    using rdesc::ETextureColorSpace;

    namespace
    {
        // Heuristic: does the imported base-color texture carry a real alpha cutout
        // mask (a meaningful fraction of texels with alpha < 0.5)? Used to infer
        // alpha-test for materials whose glTF/FBX source omitted an alpha mode.
        // Returns false for opaque textures, RGB (no alpha), and compressed/absent
        // textures — so it NEVER upgrades a genuinely-opaque material.
        bool baseColorHasTransparency(AssetManager& mgr, const asset_id_t& tex_id)
        {
            const auto* ta = mgr.fetchAssetAs<TextureAsset>(tex_id);
            if (ta == nullptr || ta->data() == nullptr) return false;
            const rdesc::Texture& t = *ta->data();
            if (t.channel() < 4 || rdesc::isCompressedFormat(t.pixelFormat()))
                return false;   // no alpha channel / not CPU-scannable -> assume opaque
            const auto* px = static_cast<const unsigned char*>(t.data());
            if (px == nullptr) return false;
            const std::size_t n =
                static_cast<std::size_t>(t.width()) * static_cast<std::size_t>(t.height());
            if (n == 0 || t.size() < n * 4) return false;
            std::size_t transparent = 0;
            for (std::size_t i = 0; i < n; ++i)
                if (px[i * 4 + 3] < 128) ++transparent;   // alpha < 0.5
            return transparent > n / 200;                 // > 0.5% transparent -> cutout mask
        }
    } // namespace

    //======================== Utility Classes & Context ========================
    /**
     * @brief Context structure for loading models from external files.
     *
     * This structure maintains the state and caches during model loading to avoid
     * duplicate imports and manage relationships between assets.
     */
    struct LoadContext {
        const std::filesystem::path&    path;    ///< Path to the model file being loaded
        ModelNode&                      model;   ///< Root model node being constructed
        const aiScene*                  scene;   ///< Assimp scene data

        // Caches to avoid duplicate imports
        std::unordered_map<std::string, asset_id_t> path_to_tex_id;       ///< Absolute path -> texture ID mapping
        std::unordered_map<std::string, asset_id_t> embedded_key_to_id;    ///< "embedded:*N" -> texture ID mapping

        // Default desc ordinal (appended once when needed).
        std::optional<std::uint32_t>                default_desc_index;

        // Asset ID lists (populated during import, transferred to ModelAsset)
        std::vector<asset_id_t> mesh_asset_ids;
        // W5b: imported material descriptors (one per material slot, parallel to
        // ModelMeshInfo::material_index) + each desc's texture-asset UUID list.
        std::vector<rdesc::ImportedMaterialDesc> material_descs;
        std::vector<std::vector<asset_id_t>>     material_desc_texture_uuids;

        // ===== Skeletal data =====
        // Populated by buildSkeleton() before processNode runs, consumed by
        // fillBoneInfluences() inside processMesh and extractAnimations() at the end.
        std::optional<asset_id_t>                   skeleton_asset_id;
        std::vector<asset_id_t>                     animation_clip_asset_ids;
        std::unordered_map<std::string, int32_t>    bone_name_to_index;

        // Import-Options bake (see ModelSerDeserConfig). import_xform = R*S
        // (linear only, no translation); import_rot = R (for normals/tangents).
        // has_import_xform is false when both are identity → bake is skipped.
        Eigen::Affine3f                             import_xform{ Eigen::Affine3f::Identity() };
        Eigen::Matrix3f                             import_rot{ Eigen::Matrix3f::Identity() };
        bool                                        has_import_xform{ false };
    };

    /**
     * @brief Imports a texture asset from file or embedded data.
     * 
     * This function handles both external texture files and embedded textures within
     * model files. It maintains a cache to avoid duplicate imports of the same texture.
     * 
     * @param ctx The loading context containing caches and scene information
     * @param absPathOrEmbeddedKey Absolute file path or embedded texture key ("embedded:*N")
     * @param embeddedIfAny Pointer to embedded texture data if available, nullptr otherwise
     * @param type The texture type identifier
     * @return Optional asset ID of the imported texture, nullopt if import failed
     */
    std::optional<asset_id_t> ModelSerDeser::importTextureAsset(
        LoadContext& ctx,
        const std::filesystem::path& absPathOrEmbeddedKey,
        const aiTexture* embeddedIfAny, 
        uint8_t type
    )
    {
        // Convert path to string once for consistency and performance
        const std::string path_key = absPathOrEmbeddedKey.string();

        // Push a deterministic per-texture seed onto the texture loader before
        // it mints (no-op when the model import isn't seeded). Use a stable,
        // machine-independent key: the embedded "*N" handle for embedded
        // textures, or the path relative to the model file for external ones.
        // Always set before use; a stale seed left on the loader from a cache
        // hit is harmless because cache hits don't mint.
        if (!config().deterministic_seed.empty())
        {
            std::string tex_key;
            if (embeddedIfAny)
            {
                tex_key = path_key; // "embedded:*N"
            }
            else
            {
                std::error_code rel_ec;
                auto rel = std::filesystem::relative(
                    absPathOrEmbeddedKey, ctx.path.parent_path(), rel_ec);
                tex_key = (!rel_ec && !rel.empty()) ? rel.generic_string() : path_key;
            }
            texture_loader_.config().deterministic_seed =
                std::format("{}|texture|{}", config().deterministic_seed, tex_key);
        }

        if (embeddedIfAny) {
            // Embedded texture cache lookup using the same key format
            if (auto it = ctx.embedded_key_to_id.find(path_key); it != ctx.embedded_key_to_id.end())
            {
                return it->second;
            }

            if (embeddedIfAny->mHeight == 0) 
            { 
                // Compressed data: pcData / mWidth bytes
                auto exp = texture_loader_.importFromMemory(embeddedIfAny->pcData, embeddedIfAny->mWidth);
                if (!exp.has_value())
                {
                    return std::nullopt;
                }
                auto& [asset, id] = exp.value();
                ctx.embedded_key_to_id.emplace(path_key, id);
                return id;
            }
            // Uncompressed pixels (rare), skip or extend as needed
            return std::nullopt;
        }
        else 
        {
            // External texture cache lookup
            if (auto it = ctx.path_to_tex_id.find(path_key); it != ctx.path_to_tex_id.end())
            {
                return it->second;
            }
            
            // Check if file exists
            if (!std::filesystem::exists(absPathOrEmbeddedKey)) {
                return std::nullopt;
            }
            
            auto exp = texture_loader_.importFromFile(absPathOrEmbeddedKey);
            if (!exp.has_value()) {
                return std::nullopt;
            }
            auto& [asset, id] = exp.value();
            ctx.path_to_tex_id.emplace(path_key, id);
            return id;
        }
    }

    /**
     * @brief Creates a texture binding from an aiMaterial texture stack.
     * 
     * This function extracts texture information from a specific texture type and index
     * within an aiMaterial and constructs a TextureBinding object with all relevant properties.
     * 
     * @param ctx The loading context containing caches and scene information
     * @param mat The aiMaterial to extract texture information from
     * @param type The texture type to query
     * @param index The texture index within the type stack
     * @param scene The aiScene containing embedded texture data
     * @return Optional TextureBinding if texture exists and is successfully processed
     */
    std::optional<rdesc::ImportedTextureRef> ModelSerDeser::makeBindingFor(LoadContext& ctx, const aiMaterial* mat, uint8_t type, unsigned index, const aiScene* scene, std::vector<asset_id_t>& texture_ids)
    {
        aiString path;
        aiTextureMapping mapping{};
        unsigned uvindex = 0;
        ai_real blend = 1.f;
        aiTextureOp op{};
        aiTextureMapMode mapmode[2]{ aiTextureMapMode_Wrap, aiTextureMapMode_Wrap };

        // C++ API: Get all texture properties at once (internally calls C version aiGetMaterialTexture)
        if (AI_SUCCESS != mat->GetTexture((aiTextureType)type, index, &path, &mapping, &uvindex, &blend, &op, mapmode))
            return std::nullopt;  // No texture of this type

        // Parse path: external file or embedded "*N"
        std::optional<asset_id_t> texture_id;
        if (path.length > 0 && path.C_Str()[0] == '*') {
            // Embedded texture
            int embedded_index = 0;
            std::from_chars(path.C_Str() + 1, path.C_Str() + path.length, embedded_index);
            if (embedded_index < 0 || embedded_index >= static_cast<int>(scene->mNumTextures)) return std::nullopt;

            const aiTexture* embedded = scene->mTextures[embedded_index];
            std::filesystem::path key = std::string("embedded:") + path.C_Str();
            texture_id = importTextureAsset(ctx, key, embedded, type);
        }
        else {
            // External file
            auto abs = ctx.path.parent_path() / path.C_Str();
            texture_id = importTextureAsset(ctx, abs, nullptr, type);
        }
        if (!texture_id) return std::nullopt;

        const uint32_t tex_idx = static_cast<uint32_t>(texture_ids.size());
        texture_ids.push_back(*texture_id);

        // The graph material path samples the bindless texture with the default
        // sampler (per-binding uv/wrap/combine are not expressed by the graph), so
        // the desc carries only the slot ordinal + colour space (set by the caller).
        return rdesc::ImportedTextureRef{ tex_idx, ETextureColorSpace::SRGB };
    }

    /**
     * @brief Determines if a material should use PBR workflow.
     * 
     * A material is considered PBR if it has any PBR-specific textures or material factors.
     * 
     * @param m The aiMaterial to analyze
     * @return true if the material appears to be PBR-based, false otherwise
     */
    static bool looksLikePBR(const aiMaterial* m) 
    {
        aiString tmp;
        // Strong PBR indicators - textures unique to PBR workflow
        if (AI_SUCCESS == m->GetTexture(aiTextureType_BASE_COLOR, 0, &tmp)) {
            return true;
        }
        if (AI_SUCCESS == m->GetTexture(aiTextureType_METALNESS, 0, &tmp)) {
            return true;
        }
        if (AI_SUCCESS == m->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &tmp)) {
            return true;
        }
        
        // NORMALS can be used in both PBR and Phong - not a strong PBR indicator
        
        if (AI_SUCCESS == m->GetTexture(aiTextureType_AMBIENT_OCCLUSION, 0, &tmp)) {
            return true;
        }
        
        // EMISSIVE can also be used in Phong - check for specific PBR factors
        float metallic = 0.f;
        float roughness = 0.f;
        bool hasMetallicFactor = (AI_SUCCESS == aiGetMaterialFloat(m, AI_MATKEY_METALLIC_FACTOR, &metallic)) && (metallic > 0.0001f);
        bool hasRoughnessFactor = (AI_SUCCESS == aiGetMaterialFloat(m, AI_MATKEY_ROUGHNESS_FACTOR, &roughness)) && (roughness > 0.0001f);
        
        // Only consider it PBR if we have meaningful metallic/roughness factors
        if (hasMetallicFactor && hasRoughnessFactor) {
            return true;
        }
        
        // Check for glTF-specific PBR material properties
        aiColor4D baseColor;
        if (AI_SUCCESS == aiGetMaterialColor(m, AI_MATKEY_BASE_COLOR, &baseColor)) {
            return true;
        }
        
        return false;
    }

    //======================== Material Setup Functions ========================

    /**
     * @brief Fills a Material object with PBR (Physically Based Rendering) properties.
     * 
     * This function extracts PBR material parameters and textures from an aiMaterial
     * and configures the provided Material object accordingly.
     * 
     * @param out The Material object to fill with PBR properties
     * @param m The source aiMaterial containing PBR data
     * @param ctx The loading context for texture imports
     * @param scene The aiScene for accessing embedded textures
     */
    void ModelSerDeser::fillPBR(rdesc::ImportedMaterialDesc& out, const aiMaterial* m, LoadContext& ctx, const aiScene* scene, std::vector<asset_id_t>& tex_ids)
    {
        // Material factors
        {
            aiColor4D c;
            if (AI_SUCCESS == aiGetMaterialColor(m, AI_MATKEY_BASE_COLOR, &c))
            {
                out.base_color = { c.r, c.g, c.b };
                out.opacity    = c.a;
            }
            aiGetMaterialFloat(m, AI_MATKEY_METALLIC_FACTOR,  &out.metallic);
            aiGetMaterialFloat(m, AI_MATKEY_ROUGHNESS_FACTOR, &out.roughness);
            aiColor4D e;
            if (AI_SUCCESS == aiGetMaterialColor(m, AI_MATKEY_COLOR_EMISSIVE, &e))
                out.emissive = { e.r, e.g, e.b };
        }

        // Double sided & Alpha mode. Assimp only exposes AI_MATKEY_GLTF_ALPHAMODE
        // when the glTF source DECLARES one; FBX/OBJ — and glTF that omits
        // alphaMode — leave it unset, so alpha_mode stays the Opaque default. We
        // track whether the source actually declared a mode so we only INFER one
        // (below, from the base-color texture's alpha) when it didn't.
        bool source_declared_alpha = false;
        {
            int twoSided = 0;
            aiGetMaterialInteger(m, AI_MATKEY_TWOSIDED, &twoSided);
            out.double_sided = (twoSided != 0);

            // glTF stores alphaMode as a STRING ("OPAQUE"/"MASK"/"BLEND") — it MUST
            // be read with aiGetMaterialString. The previous aiGetMaterialInteger
            // ALWAYS failed (type mismatch), so every glTF material silently fell
            // back to Opaque — THE bug that rendered masked/blended hair & eyelashes
            // as solid blocks despite the source declaring them correctly.
            aiString am_str;
            if (AI_SUCCESS == aiGetMaterialString(m, AI_MATKEY_GLTF_ALPHAMODE, &am_str)) {
                source_declared_alpha = true;
                const std::string_view s(am_str.C_Str(), am_str.length);
                if      (s == "MASK")  out.alpha_mode = EAlphaMode::Mask;
                else if (s == "BLEND") out.alpha_mode = EAlphaMode::Blend;
                else                   out.alpha_mode = EAlphaMode::Opaque;
            }
            aiGetMaterialFloat(m, AI_MATKEY_GLTF_ALPHACUTOFF, &out.alpha_cutoff);
        }

        // Texture slots
        if (auto b = makeBindingFor(ctx, m, aiTextureType_BASE_COLOR, 0, scene, tex_ids))
        {
            b->color_space = ETextureColorSpace::SRGB;
            out.base_color_tex = *b;
        }

        // Infer alpha-test from a transparent base-color texture when the source
        // declared NO alpha mode. Blender→glTF/FBX exports of stylized models
        // routinely ship hair/eyelash cards as Opaque while the silhouette lives
        // in the base-color alpha channel — without this they render as solid
        // blocks. The scan only flips materials whose texture genuinely has
        // transparent texels, so fully-opaque materials stay Opaque (zero false
        // positives). MaterialToGraph then wires that alpha into Opacity and the
        // baked frag's `discard` cuts the shape (cutoff = alpha_cutoff, default .5).
        if (!source_declared_alpha && out.base_color_tex
            && baseColorHasTransparency(manager(),
                                        tex_ids[out.base_color_tex->texture_index]))
        {
            out.alpha_mode = EAlphaMode::Mask;
            aiString nm; aiGetMaterialString(m, AI_MATKEY_NAME, &nm);
            std::cerr << "[import] '" << nm.C_Str() << "': undeclared alpha mode + "
                         "transparent base-color texture -> inferring alpha-test (Mask)\n";
        }
        if (auto b = makeBindingFor(ctx, m, aiTextureType_NORMALS, 0, scene, tex_ids))
        {
            b->color_space = ETextureColorSpace::DATA;
            out.normal_tex = *b;
            aiGetMaterialFloat(m, AI_MATKEY_GLTF_TEXTURE_SCALE(aiTextureType_NORMALS, 0), &out.normal_scale);
        }
        // Metallic-Roughness texture (try METALNESS first, then DIFFUSE_ROUGHNESS)
        if (auto b = makeBindingFor(ctx, m, aiTextureType_METALNESS, 0, scene, tex_ids))
        {
            b->color_space = ETextureColorSpace::DATA;
            out.metallic_roughness_tex = *b;
        }
        else if (auto b2 = makeBindingFor(ctx, m, aiTextureType_DIFFUSE_ROUGHNESS, 0, scene, tex_ids))
        {
            b2->color_space = ETextureColorSpace::DATA;
            out.metallic_roughness_tex = *b2;
        }
        if (auto b = makeBindingFor(ctx, m, aiTextureType_AMBIENT_OCCLUSION, 0, scene, tex_ids))
        {
            b->color_space = ETextureColorSpace::DATA;
            out.occlusion_tex = *b;
        }
        if (auto b = makeBindingFor(ctx, m, aiTextureType_EMISSIVE, 0, scene, tex_ids))
        {
            b->color_space = ETextureColorSpace::SRGB;
            out.emissive_tex = *b;
        }
    }

    /**
     * @brief Fills a Material object with LegacyLit (Blinn-Phong) properties.
     *
     * Extracts traditional Phong/Blinn-Phong material parameters from an aiMaterial,
     * mapping them to the new LegacyLitMaterial with BlinnPhongSpecular closure.
     */
    void ModelSerDeser::fillLegacyLit(rdesc::ImportedMaterialDesc& out, const aiMaterial* m, LoadContext& ctx, const aiScene* scene, std::vector<asset_id_t>& tex_ids)
    {
        // The Phong/Blinn-Phong specular + glossiness have no graph expression (the
        // converter collapses LegacyLit -> PBR with metallic=0 / roughness=0.5), so we
        // carry only the surface quantities the graph uses + flag is_legacy.
        out.is_legacy = true;
        {
            aiColor4D cd;
            if (AI_SUCCESS == aiGetMaterialColor(m, AI_MATKEY_COLOR_DIFFUSE, &cd))
                out.base_color = { cd.r, cd.g, cd.b };

            int twoSided = 0;
            aiGetMaterialInteger(m, AI_MATKEY_TWOSIDED, &twoSided);
            out.double_sided = (twoSided != 0);
        }

        if (auto b = makeBindingFor(ctx, m, aiTextureType_DIFFUSE, 0, scene, tex_ids)) {
            b->color_space = ETextureColorSpace::SRGB;
            out.base_color_tex = *b;
        }
        if (auto b = makeBindingFor(ctx, m, aiTextureType_NORMALS, 0, scene, tex_ids)) {
            b->color_space = ETextureColorSpace::DATA;
            out.normal_tex = *b;
        }
        if (auto b = makeBindingFor(ctx, m, aiTextureType_EMISSIVE, 0, scene, tex_ids)) {
            b->color_space = ETextureColorSpace::SRGB;
            out.emissive_tex = *b;
        }
    }

    /**
     * @brief Imports an aiMaterial and converts it to a MaterialAsset.
     * 
     * This function converts an Assimp material at the specified index into
     * a MaterialAsset and registers it with the asset manager.
     * 
     * @param ctx The loading context containing caches and scene information
     * @param matIdx The aiMaterial index in the scene
     * @return The asset ID of the imported material
     */
    std::uint32_t ModelSerDeser::importMaterial(LoadContext& ctx, unsigned matIdx)
    {
        const aiMaterial* m = ctx.scene->mMaterials[matIdx];
        rdesc::ImportedMaterialDesc desc;
        std::vector<asset_id_t> texture_ids;
        if (looksLikePBR(m))
            fillPBR(desc, m, ctx, ctx.scene, texture_ids);
        else
            fillLegacyLit(desc, m, ctx, ctx.scene, texture_ids);

        const std::uint32_t ord = static_cast<std::uint32_t>(ctx.material_descs.size());
        ctx.material_descs.push_back(std::move(desc));
        ctx.material_desc_texture_uuids.push_back(std::move(texture_ids));
        return ord;
    }

    /**
     * @brief Ensures the existence of a default Unlit material.
     * 
     * This function creates and caches a default Unlit material that can be used
     * as a fallback when no source material is available.
     * 
     * @param ctx The loading context to store the cached material ID
     * @return The asset ID of the default Unlit material
     */
    std::uint32_t ModelSerDeser::ensureDefaultUnlit(LoadContext& ctx)
    {
        if (ctx.default_desc_index)
            return *ctx.default_desc_index;
        const std::uint32_t ord = static_cast<std::uint32_t>(ctx.material_descs.size());
        ctx.material_descs.emplace_back();              // default neutral white PBR desc
        ctx.material_desc_texture_uuids.emplace_back(); // no textures
        ctx.default_desc_index = ord;
        return ord;
    }

    //======================== ModelSerDeser Main Implementation ========================
    ModelSerDeser::ModelSerDeser(std::shared_ptr<AssetManager> manager)
        : TAssetSerDeser(std::move(manager))
        , mesh_loader_(managerPtr())
        , texture_loader_(managerPtr()) {
    }

    ModelSerDeser::~ModelSerDeser() = default;

    std::string ModelSerDeser::seedFor(std::string_view kind,
                                       std::optional<std::uint64_t> index) const
    {
        if (config().deterministic_seed.empty())
            return {};
        return index
            ? std::format("{}|{}|{}", config().deterministic_seed, kind, *index)
            : std::format("{}|{}", config().deterministic_seed, kind);
    }

    lux::cxx::expected<AssetIDPair, EAssetError>
    ModelSerDeser::importFromFile(const std::filesystem::path& external_path)
    {
        auto model_root = std::make_unique<ModelNode>();
        std::vector<asset_id_t> mesh_ids, animation_clip_ids;
        std::vector<rdesc::ImportedMaterialDesc> material_descs;
        std::vector<std::vector<asset_id_t>>     material_desc_tex;
        std::optional<asset_id_t> skeleton_id;
        auto ec = loadFrom(external_path, *model_root,
                           mesh_ids, material_descs, material_desc_tex,
                           skeleton_id, animation_clip_ids);
        if (ec != EAssetError::SUCCESS)
        {
            return lux::cxx::unexpected(ec);
        }

        auto asset = manager().createAssetSeeded<ModelAsset>(
            seedFor("model"), std::move(model_root));
        auto* model_asset = static_cast<ModelAsset*>(asset.get());
        for (auto& mid : mesh_ids)
            model_asset->addMeshAssetId(mid);
        // W5b: materialAssetIds() stays EMPTY here — the editor's AssetImporter bakes a
        // MaterialAsset per imported desc and fills it. Carry the transient descs.
        for (std::size_t i = 0; i < material_descs.size(); ++i)
            model_asset->addImportedMaterialDesc(std::move(material_descs[i]),
                                                 std::move(material_desc_tex[i]));
        if (skeleton_id)
            model_asset->setSkeletonAssetId(*skeleton_id);
        for (auto& cid : animation_clip_ids)
            model_asset->addAnimationClipAssetId(cid);
        const auto& id  = asset->id();
        const auto  ptr = asset.get();

        if (!submitAsset(std::move(asset)))
            return lux::cxx::unexpected(EAssetError::ASSET_ALREADY_EXIST);

        return AssetIDPair{ ptr, id };
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  .luxmodel manifest format (info section content)
    //
    //  The model "asset" is just a graph of UUIDs into sibling files
    //  (`Mesh_*.luxasset`, `Material_*.luxasset`, etc.). We persist that graph
    //  as a small POD blob inside the AssetFileHeader's info region — the
    //  data section is unused (data_size = 0), same convention as MeshSerDeser
    //  and SkeletonSerDeser.
    //
    //  Layout:
    //      u32   format_version              // = 1
    //      u8    has_skeleton                // 1 or 0
    //      16B   skeleton_id                 // raw bytes; zero-padded when has_skeleton=0
    //      u32   mesh_count        + 16B × mesh_count
    //      u32   material_count    + 16B × material_count
    //      u32   texture_count     + 16B × texture_count  // for now empty — textures
    //                                                       are referenced indirectly via
    //                                                       MaterialAsset; persisted here so
    //                                                       a future "model directory clean-
    //                                                       up" pass can find them.
    //      u32   animation_count   + 16B × animation_count
    //
    //  All numeric fields are little-endian POD writes (no byte-swap; Lux
    //  targets LE-only — see Archive.hpp's endianness contract).
    //
    //  Forward compat: readers must check `format_version` and stop reading
    //  trailing sections introduced after their version was authored.
    // ─────────────────────────────────────────────────────────────────────────
    namespace
    {
        constexpr uint32_t kLuxModelFormatVersion = 1;

        // Append a POD value to a byte buffer at the current end.
        template <typename T>
        void appendPod(std::vector<std::byte>& buf, const T& v)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            const auto offset = buf.size();
            buf.resize(offset + sizeof(T));
            std::memcpy(buf.data() + offset, &v, sizeof(T));
        }

        void appendUuid(std::vector<std::byte>& buf, const asset_id_t& id)
        {
            const auto bytes = id.as_bytes(); // span<const std::byte, 16>
            const auto offset = buf.size();
            buf.resize(offset + bytes.size());
            std::memcpy(buf.data() + offset, bytes.data(), bytes.size());
        }

        void appendUuidVector(std::vector<std::byte>&         buf,
                              const std::vector<asset_id_t>&  ids)
        {
            appendPod<uint32_t>(buf, static_cast<uint32_t>(ids.size()));
            for (const auto& id : ids) appendUuid(buf, id);
        }

        // Cursor-based reader over a span of bytes (info section). Throws
        // EAssetError on underrun via the caller's expected<> chain.
        struct InfoCursor
        {
            const std::byte* p;
            const std::byte* end;

            bool empty(std::size_t n) const noexcept { return end - p < std::ptrdiff_t(n); }

            template <typename T>
            std::optional<T> readPod() noexcept
            {
                if (empty(sizeof(T))) return std::nullopt;
                T v;
                std::memcpy(&v, p, sizeof(T));
                p += sizeof(T);
                return v;
            }

            std::optional<asset_id_t> readUuid() noexcept
            {
                if (empty(16)) return std::nullopt;
                std::array<uint8_t, 16> bytes;
                std::memcpy(bytes.data(), p, 16);
                p += 16;
                return uuids::uuid(bytes);
            }

            bool readUuidVector(std::vector<asset_id_t>& out) noexcept
            {
                auto count = readPod<uint32_t>();
                if (!count) return false;
                out.reserve(*count);
                for (uint32_t i = 0; i < *count; ++i)
                {
                    auto u = readUuid();
                    if (!u) return false;
                    out.push_back(*u);
                }
                return true;
            }
        };

        // Whole-file slurp shared with the other SerDesers' readAll().
        EAssetError readAllStream(std::istream& ifs, std::vector<std::byte>& out)
        {
            ifs.seekg(0, std::ios::end);
            const std::streamoff n = ifs.tellg();
            if (n < 0) return EAssetError::ABNORMAL_FILE_SIZE;
            ifs.seekg(0, std::ios::beg);

            out.resize(static_cast<std::size_t>(n));
            if (n == 0) return EAssetError::SUCCESS;
            if (!ifs.read(reinterpret_cast<char*>(out.data()),
                          static_cast<std::streamsize>(out.size())))
            {
                return EAssetError::READ_FILE_FAIL;
            }
            return EAssetError::SUCCESS;
        }
    } // namespace

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    ModelSerDeser::fromLuxAssetStream(std::istream& ifs)
    {
        std::vector<std::byte> file;
        if (auto ec = readAllStream(ifs, file); ec != EAssetError::SUCCESS)
            return lux::cxx::unexpected(ec);

        AssetFileHeader header{};
        if (auto ec = loadHeaderRaw<EAssetType::MODEL>(file, header);
            ec != EAssetError::SUCCESS)
        {
            return lux::cxx::unexpected(ec);
        }
        if (header.magic_number != asset_magic_number_of<EAssetType::MODEL>::value)
            return lux::cxx::unexpected(EAssetError::WRONG_FILE_HEADER);
        if (header.info_offset != sizeof(AssetFileHeader))
            return lux::cxx::unexpected(EAssetError::WRONG_FILE_HEADER);
        if (header.data_offset != header.info_offset + header.info_size)
            return lux::cxx::unexpected(EAssetError::WRONG_FILE_HEADER);
        if (header.data_size != 0)
            return lux::cxx::unexpected(EAssetError::WRONG_FILE_HEADER);
        if (file.size() < header.data_offset)
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        InfoCursor cur{
            file.data() + header.info_offset,
            file.data() + header.info_offset + header.info_size,
        };

        const auto version = cur.readPod<uint32_t>();
        if (!version || *version != kLuxModelFormatVersion)
            return lux::cxx::unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

        const auto has_skel = cur.readPod<uint8_t>();
        if (!has_skel)
            return lux::cxx::unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
        const auto skel_id  = cur.readUuid();
        if (!skel_id)
            return lux::cxx::unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

        std::vector<asset_id_t> mesh_ids;
        std::vector<asset_id_t> mat_ids;
        std::vector<asset_id_t> tex_ids;
        std::vector<asset_id_t> anim_ids;
        if (!cur.readUuidVector(mesh_ids) ||
            !cur.readUuidVector(mat_ids)  ||
            !cur.readUuidVector(tex_ids)  ||
            !cur.readUuidVector(anim_ids))
        {
            return lux::cxx::unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);
        }

        auto ainfo = std::make_unique<AssetInfo>(header.info);
        auto model = std::make_unique<ModelAsset>(std::move(ainfo));
        for (const auto& id : mesh_ids) model->addMeshAssetId(id);
        for (const auto& id : mat_ids)  model->addMaterialAssetId(id);
        for (const auto& id : anim_ids) model->addAnimationClipAssetId(id);
        if (*has_skel) model->setSkeletonAssetId(*skel_id);
        // Textures are dangling references at this layer — Materials hold their
        // own texture id lists, so we just remember the count for diagnostics
        // (e.g. the editor can warn "manifest mentions 4 textures but only 3
        // .luxasset files survived a delete"). Not surfaced on ModelAsset yet.
        (void)tex_ids;

        return std::unique_ptr<LuxAsset>(std::move(model));
    }

    EAssetError ModelSerDeser::exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& ofs)
    {
        const auto* model = asset.as<ModelAsset>();
        if (!model)
            return EAssetError::FILE_TYPE_ERROR;

        // Compose the info section into a byte buffer first so we know its
        // size before writing the header.
        std::vector<std::byte> info;
        info.reserve(64 + 16 * (model->meshAssetIds().size()
                              + model->materialAssetIds().size()
                              + model->animationClipAssetIds().size()));

        appendPod<uint32_t>(info, kLuxModelFormatVersion);

        const bool has_skel = model->skeletonAssetId().has_value();
        appendPod<uint8_t>(info, has_skel ? uint8_t{1} : uint8_t{0});
        appendUuid(info, has_skel ? *model->skeletonAssetId() : asset_id_t{});

        appendUuidVector(info, model->meshAssetIds());
        appendUuidVector(info, model->materialAssetIds());
        appendUuidVector(info, {}); // textures — see fromLuxAssetStream note
        appendUuidVector(info, model->animationClipAssetIds());

        const std::size_t info_size = info.size();
        const std::size_t data_size = 0;

        const auto header_bytes = makeHeaderRaw<EAssetType::MODEL>(
            *model->info(), info_size, data_size);

        ofs.write(reinterpret_cast<const char*>(header_bytes.data()),
                  static_cast<std::streamsize>(header_bytes.size()));
        if (info_size > 0)
            ofs.write(reinterpret_cast<const char*>(info.data()),
                      static_cast<std::streamsize>(info_size));
        return ofs.good() ? EAssetError::SUCCESS : EAssetError::WRITE_FILE_FAIL;
    }

    EAssetError ModelSerDeser::loadFrom(const std::filesystem::path& path, ModelNode& model_root,
                                        std::vector<asset_id_t>&                  mesh_ids_out,
                                        std::vector<rdesc::ImportedMaterialDesc>& material_descs_out,
                                        std::vector<std::vector<asset_id_t>>&     material_desc_tex_out,
                                        std::optional<asset_id_t>&                skeleton_id_out,
                                        std::vector<asset_id_t>&                  animation_clip_ids_out)
    {
        Assimp::Importer importer;

        unsigned flags = aiProcess_Triangulate
            | aiProcess_JoinIdenticalVertices
            | aiProcess_GenSmoothNormals
            | aiProcess_CalcTangentSpace;

        flags |= aiProcess_FlipUVs;
        if (!config().is_right_handed)                    flags |= aiProcess_MakeLeftHanded;

        const aiScene* scene = importer.ReadFile(path.string().c_str(), flags);
        if (!scene || scene->mFlags == AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
            return EAssetError::UNKNOWN_ERROR;

        LoadContext ctx{ path, model_root, scene };

        // Resolve the Import-Options bake once. T = R*S (uniform scale folded
        // into the linear part, no translation); R alone drives normals/tangents.
        {
            const Eigen::Matrix3f R = config().pre_rotate.normalized().toRotationMatrix();
            const float s = config().uniform_scale;
            ctx.import_rot = R;
            ctx.import_xform = Eigen::Affine3f::Identity();
            ctx.import_xform.linear() = s * R;
            ctx.has_import_xform =
                !ctx.import_xform.linear().isApprox(Eigen::Matrix3f::Identity());
        }

        // Extract skeleton FIRST so processMesh's fillBoneInfluences can
        // consult ctx.bone_name_to_index for every aiBone reference.
        buildSkeleton(ctx);

        processNode(ctx, model_root, scene->mRootNode);

        // Convert aiAnimation entries (if any) AFTER processNode so
        // bone_name_to_index is fully populated. Skipped when the import opted
        // out of animations.
        if (config().import_animations)
            extractAnimations(ctx);

        mesh_ids_out           = std::move(ctx.mesh_asset_ids);
        material_descs_out     = std::move(ctx.material_descs);
        material_desc_tex_out  = std::move(ctx.material_desc_texture_uuids);
        skeleton_id_out        = ctx.skeleton_asset_id;
        animation_clip_ids_out = std::move(ctx.animation_clip_asset_ids);
        return EAssetError::SUCCESS;
    }

    void ModelSerDeser::processNode(LoadContext& ctx, ModelNode& node, const aiNode* an)
    {
        node.name = an->mName.C_Str();

        for (size_t i = 0; i < an->mNumMeshes; ++i) {
            aiMesh* ai_mesh = ctx.scene->mMeshes[an->mMeshes[i]];
            node.mesh_infos.push_back({});
            processMesh(ctx, node.mesh_infos.back(), ai_mesh);
        }
        for (size_t i = 0; i < an->mNumChildren; ++i) {
            node.children.push_back(std::make_unique<ModelNode>());
            processNode(ctx, *node.children.back(), an->mChildren[i]);
        }
    }

    void ModelSerDeser::processMesh(LoadContext& ctx, ModelMeshInfo& mi, aiMesh* mesh)
    {
        auto mesh_data = std::make_unique<Mesh>();
        mesh_data->vertices.reserve(mesh->mNumVertices);
        mesh_data->indices.reserve(mesh->mNumFaces * 3);

        for (size_t i = 0; i < mesh->mNumVertices; ++i)
        {
            mesh_data->vertices.push_back({});
            Vertex& v = mesh_data->vertices.back();
            v.position = { mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z };

            if (mesh->mNormals)
            {
                v.normal = { mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z };
            }

            if (mesh->mTextureCoords[0])
            {
                v.uv = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };
            }

            if (mesh->mTangents)
            {
                v.tangent = { mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z };
            }

            if (mesh->mBitangents)
            {
                v.bitangent = { mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z };
            }

            // Initialise bone influences to "no skinning". fillBoneInfluences
            // overwrites these for any vertex an aiBone references.
            for (int b = 0; b < lux::rdesc::max_bone_influence; ++b)
            {
                v.bone.bone_ids[b] = -1;
                v.bone.weights[b]  = 0.0f;
            }
        }

        // Fill per-vertex bone influences if this mesh is skinned and we
        // built a skeleton from the scene.
        if (mesh->HasBones() && ctx.skeleton_asset_id.has_value())
        {
            fillBoneInfluences(ctx, mesh, mesh_data->vertices);
        }

        // Bake the Import-Options transform into STATIC meshes. Skinned meshes
        // (HasBones) are rotated via Skeleton::global_transform in buildSkeleton
        // instead — baking their bind-space vertices would fight skinning. Done
        // before the AABB below so the stored bounds match the stored geometry.
        if (ctx.has_import_xform && !mesh->HasBones())
        {
            for (Vertex& v : mesh_data->vertices)
            {
                v.position  = ctx.import_xform * v.position;        // R*S
                v.normal    = (ctx.import_rot * v.normal).normalized();
                v.tangent   = ctx.import_rot * v.tangent;           // R preserves length
                v.bitangent = ctx.import_rot * v.bitangent;
            }
        }

        for (size_t i = 0; i < mesh->mNumFaces; ++i)
        {
            aiFace& f = mesh->mFaces[i];
            for (size_t j = 0; j < f.mNumIndices; ++j)
            {
                mesh_data->indices.push_back(f.mIndices[j]);
            }
        }

        // Compute AABB from vertex positions during asset loading.
        if (!mesh_data->vertices.empty())
        {
            lux::math::AABB aabb;
            for (const auto& v : mesh_data->vertices)
                aabb.merge(v.position);
            mesh_data->bounds = aabb;
        }

        // Auto-generate a discrete LOD chain (static meshes only). meshopt_simplify
        // reduces the index list over the SAME vertices, so LODs share LOD0's VBO
        // and add only small index buffers. Skinned meshes keep a single LOD —
        // simplifying bind-pose geometry fights skinning at the LOD seams.
        static constexpr std::size_t kLodMinIndexCount = 768; // <256 tris: not worth LODs
        if (!mesh->HasBones() && mesh_data->indices.size() >= kLodMinIndexCount)
        {
            static constexpr float kLodRatios[] = { 0.5f, 0.25f, 0.1f };
            static constexpr float kLodMaxError = 0.05f;
            const auto&       base   = mesh_data->indices;
            const float*      pos    = mesh_data->vertices[0].position.data();
            const std::size_t vcount = mesh_data->vertices.size();
            std::size_t       prev   = base.size();
            for (float ratio : kLodRatios)
            {
                const std::size_t target =
                    std::max<std::size_t>(3, static_cast<std::size_t>(base.size() * ratio));
                if (target >= prev) break;
                std::vector<uint32_t> lod(base.size());
                float err = 0.0f;
                const std::size_t n = meshopt_simplify(
                    lod.data(), base.data(), base.size(),
                    pos, vcount, sizeof(Vertex),
                    target, kLodMaxError, /*options=*/0u, &err);
                if (n == 0 || n >= prev) break;   // no meaningful reduction → stop
                lod.resize(n);
                mesh_data->lods.push_back({ std::move(lod), err });
                prev = n;
            }
        }

        uint32_t mesh_idx = static_cast<uint32_t>(ctx.mesh_asset_ids.size());
        auto mesh_asset = manager().createAssetSeeded<MeshAsset>(
            seedFor("mesh", mesh_idx), std::move(mesh_data));
        ctx.mesh_asset_ids.push_back(mesh_asset->id());
        mi.mesh_index = mesh_idx;
        mi.name = mesh->mName.C_Str();
        manager().registerAsset(std::move(mesh_asset));

        // Apply material — importMaterial/ensureDefaultUnlit append an
        // ImportedMaterialDesc to the context and return its ordinal (W5b).
        const std::uint32_t mat_idx =
            (mesh->mMaterialIndex >= 0) ? importMaterial(ctx, mesh->mMaterialIndex)
                                        : ensureDefaultUnlit(ctx);
        mi.material_index = mat_idx;
    }

    //======================== Skeletal extraction ========================
    namespace
    {
        // Convert Assimp's column-major aiMatrix4x4 to Eigen::Affine3f.
        // Assimp stores mat[row][col] (row-major access), Eigen Affine3f's data()
        // is column-major. Iterate columns explicitly.
        inline Eigen::Affine3f toEigenAffine(const aiMatrix4x4& m)
        {
            Eigen::Matrix4f e;
            e <<
                m.a1, m.a2, m.a3, m.a4,
                m.b1, m.b2, m.b3, m.b4,
                m.c1, m.c2, m.c3, m.c4,
                m.d1, m.d2, m.d3, m.d4;
            Eigen::Affine3f a;
            a.matrix() = e;
            return a;
        }

        // Recursive walker that finds the aiNode whose mName equals `name`.
        // Returns nullptr if not found.
        const aiNode* findNodeByName(const aiNode* root, const std::string& name)
        {
            if (!root) return nullptr;
            if (root->mName.C_Str() == name) return root;
            for (uint32_t i = 0; i < root->mNumChildren; ++i)
            {
                if (auto* hit = findNodeByName(root->mChildren[i], name))
                    return hit;
            }
            return nullptr;
        }
    } // anonymous

    void ModelSerDeser::buildSkeleton(LoadContext& ctx)
    {
        // 1. Collect the set of bone names referenced by any aiMesh.
        std::unordered_map<std::string, aiMatrix4x4> bone_offsets; // name -> inv_bind_world
        for (uint32_t mi = 0; mi < ctx.scene->mNumMeshes; ++mi)
        {
            const aiMesh* m = ctx.scene->mMeshes[mi];
            for (uint32_t bi = 0; bi < m->mNumBones; ++bi)
            {
                const aiBone* b = m->mBones[bi];
                bone_offsets.try_emplace(b->mName.C_Str(), b->mOffsetMatrix);
            }
        }
        if (bone_offsets.empty())
            return; // static mesh — nothing to do

        // Handedness guard: this hand-written extraction copies
        // aiBone::mOffsetMatrix and node transforms verbatim and does NOT
        // compensate for aiProcess_MakeLeftHanded (applied when
        // !config().is_right_handed). Skinned + left-handed import is
        // unvalidated and would silently mis-skin — fail loudly instead.
        if (!config().is_right_handed)
        {
            std::cerr << "[ModelSerDeser] WARNING: skinned import with left-handed "
                         "conversion (is_right_handed=false) is unvalidated; skinning "
                         "may be incorrect.\n";
            assert(config().is_right_handed &&
                   "skinned import + MakeLeftHanded is unsupported");
        }

        // 2. Walk the aiNode tree depth-first; visit a node *after* its parent
        //    (BFS-style queue) so parent_index < self_index for every bone.
        //    Only emit a Bone_t if the node's name matches a referenced bone.
        auto skeleton = std::make_unique<Skeleton>();
        skeleton->bones.reserve(bone_offsets.size());

        std::unordered_map<std::string, int32_t> node_name_to_bone_index;
        // node_name_to_node parent index lookup: we record the node's *bone index*
        // (or -1 if not a bone) so children can resolve their parent_index.
        std::unordered_map<const aiNode*, int32_t> node_to_bone_index;

        // BFS via deque-style processing — std::function recursion is simpler.
        // Thread an `accum` = product of mTransformation of the non-bone
        // nodes (armature / empties) below the scene root, to recover the transform
        // sitting between the scene root and the skeleton root bone. The scene-root
        // transform itself cancels against Assimp's globalInverse, so the root call
        // passes Identity (root excluded). The chain is captured into
        // skeleton->global_transform at the FIRST bone and applied to root bones at
        // runtime (buildSkinningMatrices). Identity for the common case → no change.
        bool first_bone_seen = false;
        std::function<void(const aiNode*, int32_t, const Eigen::Affine3f&)> walk =
            [&](const aiNode* n, int32_t parent_bone_index, const Eigen::Affine3f& accum)
        {
            int32_t self_bone_index = -1;
            const std::string node_name = n->mName.C_Str();
            auto it = bone_offsets.find(node_name);
            if (it != bone_offsets.end())
            {
                Eigen::Affine3f node_local = toEigenAffine(n->mTransformation);
                if (!first_bone_seen)
                {
                    // The non-bone chain above the skeleton root is a constant
                    // (never animated) prefix → store it, do NOT fold into bind_local.
                    skeleton->global_transform = accum;
                    first_bone_seen = true;
                }
                else if (!accum.matrix().isApprox(Eigen::Matrix4f::Identity()))
                {
                    // Rare: a non-bone node sits BETWEEN two bones. Best-effort fold
                    // into this bone's rest local; animated result may be approximate.
                    std::cerr << "[ModelSerDeser] WARNING: non-bone node transform between "
                                 "bones near '" << node_name << "' folded into bind_local; "
                                 "animated result may be approximate.\n";
                    node_local = accum * node_local;
                }
                self_bone_index = static_cast<int32_t>(skeleton->bones.size());
                Bone_t b;
                b.name           = node_name;
                b.parent_index   = parent_bone_index;
                b.bind_local     = node_local;
                b.inv_bind_world = toEigenAffine(it->second);
                skeleton->bones.push_back(std::move(b));
                node_name_to_bone_index.emplace(node_name, self_bone_index);
            }
            node_to_bone_index.emplace(n, self_bone_index);
            // Children inherit our bone_index as their parent if we are a bone;
            // otherwise pass through whatever ancestor was a bone.
            const int32_t child_parent =
                (self_bone_index >= 0) ? self_bone_index : parent_bone_index;
            // Children's accum: a bone (or the scene root, whose transform cancels
            // against globalInverse) resets it to Identity; any other non-bone node
            // multiplies its own transform in.
            Eigen::Affine3f child_accum =
                (self_bone_index >= 0 || n == ctx.scene->mRootNode)
                    ? Eigen::Affine3f::Identity()
                    : Eigen::Affine3f(accum * toEigenAffine(n->mTransformation));
            for (uint32_t i = 0; i < n->mNumChildren; ++i)
                walk(n->mChildren[i], child_parent, child_accum);
        };
        walk(ctx.scene->mRootNode, /*parent_bone_index=*/-1, Eigen::Affine3f::Identity());

        ctx.bone_name_to_index = std::move(node_name_to_bone_index);

        // Bake the Import-Options transform into skinned geometry by folding it
        // into the armature prefix: buildSkinningMatrices left-multiplies
        // global_transform onto every root bone, so it prefixes every bone's
        // world transform — hence the whole skinned result becomes T * result.
        // This needs NO change to vertices, inv_bind_world, or animation tracks.
        if (ctx.has_import_xform)
            skeleton->global_transform = ctx.import_xform * skeleton->global_transform;

        auto skel_asset = manager().createAssetSeeded<SkeletonAsset>(
            seedFor("skeleton"), std::move(skeleton));
        const asset_id_t id = skel_asset->id();
        manager().registerAsset(std::move(skel_asset));
        ctx.skeleton_asset_id = id;
    }

    void ModelSerDeser::fillBoneInfluences(LoadContext& ctx,
                                           aiMesh* mesh,
                                           std::vector<Vertex>& verts)
    {
        // Per-vertex accumulator of (bone_index, weight) — uncapped during
        // collection. After collection, sort desc by weight, keep top-4,
        // renormalize.
        std::vector<std::vector<std::pair<int32_t, float>>>
            per_vert(mesh->mNumVertices);

        for (uint32_t bi = 0; bi < mesh->mNumBones; ++bi)
        {
            const aiBone* b = mesh->mBones[bi];
            auto it = ctx.bone_name_to_index.find(b->mName.C_Str());
            if (it == ctx.bone_name_to_index.end())
                continue; // bone not in our skeleton — Assimp anomaly, skip
            const int32_t bone_index = it->second;

            for (uint32_t wi = 0; wi < b->mNumWeights; ++wi)
            {
                const aiVertexWeight& w = b->mWeights[wi];
                if (w.mVertexId >= mesh->mNumVertices) continue;
                if (w.mWeight <= 0.0f)                continue;
                per_vert[w.mVertexId].push_back({bone_index, w.mWeight});
            }
        }

        for (uint32_t v = 0; v < mesh->mNumVertices; ++v)
        {
            auto& influences = per_vert[v];
            if (influences.empty()) continue;

            std::sort(influences.begin(), influences.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });

            // Keep top-4, sum weights, renormalize.
            const int kept = std::min<int>(lux::rdesc::max_bone_influence,
                                           static_cast<int>(influences.size()));
            float sum = 0.0f;
            for (int i = 0; i < kept; ++i) sum += influences[i].second;
            const float inv_sum = (sum > 0.0f) ? (1.0f / sum) : 0.0f;

            Vertex& vert = verts[v];
            for (int i = 0; i < kept; ++i)
            {
                vert.bone.bone_ids[i] = influences[i].first;
                vert.bone.weights[i]  = influences[i].second * inv_sum;
            }
        }
    }

    void ModelSerDeser::extractAnimations(LoadContext& ctx)
    {
        if (ctx.scene->mNumAnimations == 0) return;
        if (!ctx.skeleton_asset_id.has_value()) return; // no rig to target

        for (uint32_t ai = 0; ai < ctx.scene->mNumAnimations; ++ai)
        {
            const aiAnimation* a = ctx.scene->mAnimations[ai];
            const float ticks_per_sec =
                (a->mTicksPerSecond > 0.0) ? static_cast<float>(a->mTicksPerSecond) : 25.0f;
            const float seconds_per_tick = 1.0f / ticks_per_sec;

            auto clip = std::make_unique<AnimationClip>();
            clip->name     = a->mName.C_Str();
            clip->duration = static_cast<float>(a->mDuration) * seconds_per_tick;
            clip->loop     = true; // import default; runtime may override per-instance
            clip->tracks.reserve(a->mNumChannels);

            for (uint32_t ci = 0; ci < a->mNumChannels; ++ci)
            {
                const aiNodeAnim* na = a->mChannels[ci];
                auto it = ctx.bone_name_to_index.find(na->mNodeName.C_Str());
                if (it == ctx.bone_name_to_index.end())
                    continue; // channel targets a non-bone node — skip

                BoneTrack t;
                t.bone_index = it->second;

                t.times_t.reserve(na->mNumPositionKeys);
                t.translations.reserve(na->mNumPositionKeys);
                for (uint32_t k = 0; k < na->mNumPositionKeys; ++k)
                {
                    const auto& key = na->mPositionKeys[k];
                    t.times_t.push_back(static_cast<float>(key.mTime) * seconds_per_tick);
                    t.translations.emplace_back(
                        key.mValue.x, key.mValue.y, key.mValue.z);
                }

                t.times_r.reserve(na->mNumRotationKeys);
                t.rotations.reserve(na->mNumRotationKeys);
                for (uint32_t k = 0; k < na->mNumRotationKeys; ++k)
                {
                    const auto& key = na->mRotationKeys[k];
                    t.times_r.push_back(static_cast<float>(key.mTime) * seconds_per_tick);
                    // Eigen::Quaternionf takes (w, x, y, z).
                    t.rotations.emplace_back(
                        key.mValue.w, key.mValue.x, key.mValue.y, key.mValue.z);
                }

                t.times_s.reserve(na->mNumScalingKeys);
                t.scales.reserve(na->mNumScalingKeys);
                for (uint32_t k = 0; k < na->mNumScalingKeys; ++k)
                {
                    const auto& key = na->mScalingKeys[k];
                    t.times_s.push_back(static_cast<float>(key.mTime) * seconds_per_tick);
                    t.scales.emplace_back(
                        key.mValue.x, key.mValue.y, key.mValue.z);
                }

                clip->tracks.push_back(std::move(t));
            }

            if (clip->tracks.empty()) continue; // nothing useful in this aiAnimation

            auto clip_asset = manager().createAssetSeeded<AnimationClipAsset>(
                seedFor("clip", ctx.animation_clip_asset_ids.size()), std::move(clip));
            const asset_id_t id = clip_asset->id();
            manager().registerAsset(std::move(clip_asset));
            ctx.animation_clip_asset_ids.push_back(id);
        }
    }

} // namespace lux::asset
