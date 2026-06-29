#include <lux/engine/editor/thumbnail/ThumbnailSpecProvider.hpp>
#include <lux/engine/editor/thumbnail/PreviewScene.hpp>

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/MeshAsset.hpp>
#include <lux/engine/asset/MaterialAsset.hpp>
#include <lux/engine/asset/TextureAsset.hpp>
#include <lux/engine/asset/ModelAsset.hpp>
#include <lux/engine/editor/content/ModelMaterialResolve.hpp>   // resolveModelSubmeshes

#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Texture.hpp>

#include <Eigen/Core>

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace lux::editor
{
    namespace
    {
        // ── Asset → CPU spec helpers (pure, no GPU / session) ────────────────
        lux::math::AABB computeBounds(const lux::rdesc::Mesh& mesh)
        {
            if (mesh.bounds.has_value())
                return *mesh.bounds;
            if (mesh.vertices.empty())
                return lux::math::AABB(Eigen::Vector3f(-0.5f, -0.5f, -0.5f),
                                       Eigen::Vector3f( 0.5f,  0.5f,  0.5f));
            lux::math::AABB b; // default: inverted, ready to merge
            for (const auto& v : mesh.vertices)
                b.merge(v.position);
            return b;
        }

        // Skinned-mesh detection for the unified vertex layout: a mesh is
        // skinned when ANY vertex carries a nonzero bone weight. Returns the
        // identity-palette size (max referenced bone id + 1; 0 = not skinned)
        // so the service can render the BIND POSE.
        std::uint32_t skinnedBoneCount(const lux::rdesc::Mesh& mesh)
        {
            int max_bone = -1;
            for (const auto& v : mesh.vertices)
                for (std::uint8_t k = 0; k < lux::rdesc::max_bone_influence; ++k)
                    if (v.bone.weights[k] > 0.0f && v.bone.bone_ids[k] >= 0)
                        max_bone = std::max(max_bone, v.bone.bone_ids[k]);
            return static_cast<std::uint32_t>(max_bone + 1);
        }

        // Convert an uncompressed texture's mip 0 to tightly-packed RGBA8.
        // Compressed / float formats are deferred to a GPU sampling path.
        std::optional<std::vector<std::byte>>
        textureToRgba8(const lux::rdesc::Texture& tex, std::uint32_t& out_w, std::uint32_t& out_h)
        {
            const int w = tex.width(), h = tex.height();
            if (w <= 0 || h <= 0) return std::nullopt;
            const auto* src = static_cast<const std::byte*>(tex.data());
            if (!src) return std::nullopt;

            out_w = static_cast<std::uint32_t>(w);
            out_h = static_cast<std::uint32_t>(h);
            const std::size_t n = static_cast<std::size_t>(w) * h;
            std::vector<std::byte> rgba(n * 4);

            using F = lux::rdesc::ETexturePixelFormat;
            switch (tex.pixelFormat())
            {
            case F::RGBA8_UNORM:
            case F::RGBA8_SRGB:
                if (tex.size() < n * 4) return std::nullopt;
                std::memcpy(rgba.data(), src, n * 4);
                break;
            case F::RG8_UNORM:
                if (tex.size() < n * 2) return std::nullopt;
                for (std::size_t i = 0; i < n; ++i)
                {
                    rgba[i*4+0] = src[i*2+0]; rgba[i*4+1] = src[i*2+1];
                    rgba[i*4+2] = std::byte{0}; rgba[i*4+3] = std::byte{255};
                }
                break;
            case F::R8_UNORM:
                if (tex.size() < n) return std::nullopt;
                for (std::size_t i = 0; i < n; ++i)
                {
                    const std::byte r = src[i];
                    rgba[i*4+0] = r; rgba[i*4+1] = r; rgba[i*4+2] = r; rgba[i*4+3] = std::byte{255};
                }
                break;
            default:
                return std::nullopt; // BCn / float — needs GPU decode (follow-up)
            }
            return rgba;
        }

        // ── Spec providers ───────────────────────────────────────────────────
        class TextureThumbnailSpecProvider final : public IThumbnailSpecProvider
        {
        public:
            ThumbnailSpec buildSpec(lux::asset::AssetManager& assets,
                                    const PreviewScene&,
                                    const lux::asset::asset_id_t& id,
                                    const ThumbnailLoadFn& request_load) override
            {
                ThumbnailSpec spec;
                const auto* asset = assets.fetchAssetAs<lux::asset::TextureAsset>(id);
                if (!asset) return spec;                                  // unknown id
                if (!asset->data()) { request_load(id); spec.pending = true; return spec; }
                std::uint32_t w = 0, h = 0;
                auto rgba = textureToRgba8(*asset->data(), w, h);
                if (!rgba) return spec;
                spec.has_cpu_pixels = true;
                spec.rgba8          = std::move(*rgba);
                spec.cpu_width      = w;
                spec.cpu_height     = h;
                spec.valid          = true;
                return spec;
            }
        };

        class MeshThumbnailSpecProvider final : public IThumbnailSpecProvider
        {
        public:
            ThumbnailSpec buildSpec(lux::asset::AssetManager& assets,
                                    const PreviewScene&,
                                    const lux::asset::asset_id_t& id,
                                    const ThumbnailLoadFn& request_load) override
            {
                ThumbnailSpec spec;
                const auto* asset = assets.fetchAssetAs<lux::asset::MeshAsset>(id);
                if (!asset) return spec;                                  // unknown id
                if (!asset->data()) { request_load(id); spec.pending = true; return spec; }
                const auto& mesh = *asset->data();
                ThumbnailInstanceSpec inst{&mesh, {}};       // default grey
                inst.bone_count = skinnedBoneCount(mesh);
                inst.skinned    = inst.bone_count > 0;
                spec.instances.push_back(inst);
                spec.bounds = computeBounds(mesh);
                spec.valid  = true;
                return spec;
            }
        };

        class ModelThumbnailSpecProvider final : public IThumbnailSpecProvider
        {
        public:
            ThumbnailSpec buildSpec(lux::asset::AssetManager& assets,
                                    const PreviewScene&,
                                    const lux::asset::asset_id_t& id,
                                    const ThumbnailLoadFn& request_load) override
            {
                ThumbnailSpec spec;
                const auto* model = assets.fetchAssetAs<lux::asset::ModelAsset>(id);
                if (!model || model->meshAssetIds().empty()) return spec;

                // 大世界 W2b:模型的 mesh/material/纹理 子资产现在都是按需流送的空壳。先确保
                // 它们数据就位(缺则请求加载 + 报 pending 让服务稍后重试),否则首帧只能渲染
                // 残缺/无纹理模型。材质的纹理槽需等材质本身就位后才能读到 —— 跨重试自然收敛:
                // 先载 mesh+material,材质就位后再载其纹理,全齐才 build。
                bool deps_pending = false;
                for (const auto& mid : model->meshAssetIds())
                    if (!mid.is_nil() && !assets.hasData(mid)) { request_load(mid); deps_pending = true; }
                for (const auto& mid : model->materialAssetIds())
                {
                    if (mid.is_nil()) continue;
                    const auto* ma = assets.fetchAssetAs<lux::asset::MaterialAsset>(mid);
                    if (!ma || !ma->data()) { request_load(mid); deps_pending = true; continue; }
                    const lux::asset::MaterialData& mp = *ma->data();
                    for (std::uint32_t s = 0; s < lux::asset::MaterialData::kMaxTextures; ++s)
                    {
                        const auto& tid = mp.texture_slot_ids[s];
                        if (!tid.is_nil() && !assets.hasData(tid)) { request_load(tid); deps_pending = true; }
                    }
                }
                if (deps_pending) { spec.pending = true; return spec; }

                // mesh -> material from the node tree (with positional + first-material
                // fallback for a disk-reloaded model whose node tree is gone): the SAME
                // resolution the spawn path uses, so a model's thumbnail and its spawned
                // entity wear the same materials. See resolveModelSubmeshes — its fallback
                // is what fixes the "model thumbnail has no material on second open" bug.
                const lux::editor::ModelSubmeshResolve submesh =
                    lux::editor::resolveModelSubmeshes(*model);
                auto materialFor = [&](std::size_t i) -> lux::asset::asset_id_t
                {
                    return i < submesh.material.size() ? submesh.material[i]
                                                       : lux::asset::asset_id_t{};
                };

                lux::math::AABB combined; // inverted → merge
                for (std::size_t i = 0; i < model->meshAssetIds().size(); ++i)
                {
                    const auto* ma = assets.fetchAssetAs<lux::asset::MeshAsset>(
                        model->meshAssetIds()[i]);
                    if (!ma || !ma->data()) continue;
                    ThumbnailInstanceSpec inst{ ma->data(), materialFor(i) };
                    inst.bone_count = skinnedBoneCount(*ma->data());
                    inst.skinned    = inst.bone_count > 0;
                    spec.instances.push_back(inst);
                    combined.merge(computeBounds(*ma->data()));
                }
                if (spec.instances.empty()) return spec;
                spec.bounds = combined;
                spec.valid  = true;
                return spec;
            }
        };

        class GraphMaterialThumbnailSpecProvider final : public IThumbnailSpecProvider
        {
        public:
            ThumbnailSpec buildSpec(lux::asset::AssetManager& assets,
                                    const PreviewScene& preview,
                                    const lux::asset::asset_id_t& id,
                                    const ThumbnailLoadFn& request_load) override
            {
                ThumbnailSpec spec;
                const auto* asset = assets.fetchAssetAs<lux::asset::MaterialAsset>(id);
                if (!asset) return spec;                                  // unknown id
                if (!asset->data()) { request_load(id); spec.pending = true; return spec; }
                if (preview.sphereMesh().is_null()) return spec;

                // 大世界 W2b:材质引用的纹理槽也是流送空壳 —— 缺则请求加载 + 报 pending,
                // 让缩略图等纹理就位再渲染(否则标准材质缩略图首版无纹理)。
                const lux::asset::MaterialData& payload = *asset->data();
                bool tex_pending = false;
                for (std::uint32_t s = 0; s < lux::asset::MaterialData::kMaxTextures; ++s)
                {
                    const auto& tid = payload.texture_slot_ids[s];
                    if (!tid.is_nil() && !assets.hasData(tid)) { request_load(tid); tex_pending = true; }
                }
                if (tex_pending) { spec.pending = true; return spec; }

                // Just reference the asset by id — the service loads the baked
                // SPIR-V + params + texture-slot UUIDs, compiles the frags, resolves
                // textures to bindless, and uploads via uploadGraphMaterial so the
                // preview sphere wears the real graph material (its own PSO).
                // mesh_data == nullptr → the service binds PreviewScene's sphere.
                ThumbnailInstanceSpec inst;
                inst.graph_material_id = id;
                spec.instances.push_back(std::move(inst));
                spec.bounds = lux::math::AABB(Eigen::Vector3f(-0.5f, -0.5f, -0.5f),
                                              Eigen::Vector3f( 0.5f,  0.5f,  0.5f));
                spec.valid  = true;
                return spec;
            }
        };
    } // namespace

    void ThumbnailSpecProviderRegistry::registerProvider(
        lux::asset::EAssetType type, std::unique_ptr<IThumbnailSpecProvider> provider)
    {
        providers_[static_cast<int>(type)] = std::move(provider);
    }

    IThumbnailSpecProvider* ThumbnailSpecProviderRegistry::get(lux::asset::EAssetType type) const noexcept
    {
        const auto it = providers_.find(static_cast<int>(type));
        return it == providers_.end() ? nullptr : it->second.get();
    }

    ThumbnailSpecProviderRegistry makeDefaultThumbnailSpecProviders()
    {
        ThumbnailSpecProviderRegistry reg;
        reg.registerProvider(lux::asset::EAssetType::TEXTURE,        std::make_unique<TextureThumbnailSpecProvider>());
        reg.registerProvider(lux::asset::EAssetType::MESH,           std::make_unique<MeshThumbnailSpecProvider>());
        reg.registerProvider(lux::asset::EAssetType::MODEL,          std::make_unique<ModelThumbnailSpecProvider>());
        reg.registerProvider(lux::asset::EAssetType::MATERIAL, std::make_unique<GraphMaterialThumbnailSpecProvider>());
        return reg;
    }

} // namespace lux::editor
