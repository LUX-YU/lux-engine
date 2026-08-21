#include <lux/engine/editor/thumbnail/ThumbnailSpecProvider.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialInstanceAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
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
        void reportMissing(
            ThumbnailSpec& spec,
            const lux::asset::asset_id_t& id)
        {
            if (id.is_nil())
                return;
            spec.pending = true;
            spec.missing_assets.push_back(id);
        }

        class TextureThumbnailSpecProvider final : public IThumbnailSpecProvider
        {
        public:
            ThumbnailSpec buildSpec(lux::asset::AssetManager& assets,
                                    const lux::asset::asset_id_t&,
                                    const lux::asset::asset_id_t& id) override
            {
                ThumbnailSpec spec;
                const auto* asset = assets.fetchAssetAs<lux::asset::TextureAsset>(id);
                if (!asset) return spec;                                  // unknown id
                if (!asset->data()) { reportMissing(spec, id); return spec; }
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
                                    const lux::asset::asset_id_t&,
                                    const lux::asset::asset_id_t& id) override
            {
                ThumbnailSpec spec;
                const auto* asset = assets.fetchAssetAs<lux::asset::MeshAsset>(id);
                if (!asset) return spec;                                  // unknown id
                // 网格数据本体只为算 bounds(上传归资源解析器,它自己会再取)。
                if (!asset->data()) { reportMissing(spec, id); return spec; }
                spec.instances.push_back(ThumbnailInstanceSpec{id, {}});  // 无材质 → PreviewGrey
                spec.bounds = computeBounds(*asset->data());
                spec.valid  = true;
                return spec;
            }
        };

        class ModelThumbnailSpecProvider final : public IThumbnailSpecProvider
        {
        public:
            ThumbnailSpec buildSpec(lux::asset::AssetManager& assets,
                                    const lux::asset::asset_id_t&,
                                    const lux::asset::asset_id_t& id) override
            {
                ThumbnailSpec spec;
                const auto* model = assets.fetchAssetAs<lux::asset::ModelAsset>(id);
                if (!model) return spec;
                if (!model->hasData()) { reportMissing(spec, id); return spec; }
                if (model->meshAssetIds().empty()) return spec;

                // Model mesh/material/texture sub-assets are now on-demand
                // streaming shells. First make sure their data is resident
                // (if missing, request a load and report pending so the
                // service retries later) — the mesh data is needed for bounds
                // and the material's texture slots can only be read once the
                // material itself is resident, so this converges naturally
                // across retries: load mesh+material first, then load its
                // textures once the material is resident, and only build once
                // everything is in place.
                bool deps_pending = false;
                for (const auto& mid : model->meshAssetIds())
                    if (!mid.is_nil() && !assets.hasData(mid))
                    { reportMissing(spec, mid); deps_pending = true; }
                for (const auto& mid : model->materialAssetIds())
                {
                    if (mid.is_nil()) continue;
                    const auto* ma = assets.fetchAssetAs<lux::asset::MaterialAsset>(mid);
                    if (!ma || !ma->data())
                    { reportMissing(spec, mid); deps_pending = true; continue; }
                    const lux::asset::MaterialData& mp = *ma->data();
                    for (std::uint32_t s = 0; s < lux::asset::MaterialData::kMaxTextures; ++s)
                    {
                        const auto& tid = mp.texture_slot_ids[s];
                        if (!tid.is_nil() && !assets.hasData(tid))
                        { reportMissing(spec, tid); deps_pending = true; }
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
                    const auto& mesh_id = model->meshAssetIds()[i];
                    const auto* ma = assets.fetchAssetAs<lux::asset::MeshAsset>(mesh_id);
                    if (!ma || !ma->data()) continue;
                    spec.instances.push_back(
                        ThumbnailInstanceSpec{mesh_id, materialFor(i)});
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
                                    const lux::asset::asset_id_t& sphere_mesh_id,
                                    const lux::asset::asset_id_t& id) override
            {
                ThumbnailSpec spec;
                const auto* asset = assets.fetchAssetAs<lux::asset::MaterialAsset>(id);
                if (!asset) return spec;                                  // unknown id
                if (!asset->data()) { reportMissing(spec, id); return spec; }
                if (sphere_mesh_id.is_nil()) return spec;   // builtin sphere absent

                // The texture slots a material references are also
                // streaming shells — if missing, request a load and report
                // pending, so the thumbnail waits for the texture to become
                // resident before rendering (otherwise a standard-material
                // thumbnail's first version would render without textures).
                const lux::asset::MaterialData& payload = *asset->data();
                bool tex_pending = false;
                for (std::uint32_t s = 0; s < lux::asset::MaterialData::kMaxTextures; ++s)
                {
                    const auto& tid = payload.texture_slot_ids[s];
                    if (!tid.is_nil() && !assets.hasData(tid))
                    { reportMissing(spec, tid); tex_pending = true; }
                }
                if (tex_pending) { spec.pending = true; return spec; }

                // 球 + 该材质 —— job 实体 = MeshComponent{sphere, material}。
                // 上传/编译/贴图解析都归资源解析器(ensureMaterial 按资产类型分派),
                // 这里只引用 id。
                spec.instances.push_back(ThumbnailInstanceSpec{sphere_mesh_id, id});
                spec.bounds = lux::math::AABB(Eigen::Vector3f(-0.5f, -0.5f, -0.5f),
                                              Eigen::Vector3f( 0.5f,  0.5f,  0.5f));
                spec.valid  = true;
                return spec;
            }
        };

        // 材质实例:与模板 provider 同构(球 + 资产 id;上传/父级编译/贴图解析
        // 都归 ensureMaterial 的类型分派 —— ensureMaterialInstance 自己解析
        // parent,这里不用)。差异只在**驻留门**:实例数据 → 父材质数据 →
        // 有效贴图槽(override 的槽用实例的,其余用父级的)三级都要就绪才发,
        // 否则首版缩略图会在贴图/父级未驻留时渲出残样。
        class MaterialInstanceThumbnailSpecProvider final : public IThumbnailSpecProvider
        {
        public:
            ThumbnailSpec buildSpec(lux::asset::AssetManager& assets,
                                    const lux::asset::asset_id_t& sphere_mesh_id,
                                    const lux::asset::asset_id_t& id) override
            {
                ThumbnailSpec spec;
                const auto* inst = assets.fetchAssetAs<lux::asset::MaterialInstanceAsset>(id);
                if (!inst) return spec;                                   // unknown id
                if (!inst->data()) { reportMissing(spec, id); return spec; }
                if (sphere_mesh_id.is_nil()) return spec;   // builtin sphere absent

                const auto& idata = *inst->data();
                if (idata.parent_material_id.is_nil()) return spec;       // 孤儿实例:回落字形

                const auto* parent =
                    assets.fetchAssetAs<lux::asset::MaterialAsset>(idata.parent_material_id);
                if (!parent || !parent->data())
                { reportMissing(spec, idata.parent_material_id); return spec; }

                bool tex_pending = false;
                for (std::uint32_t s = 0; s < lux::asset::MaterialData::kMaxTextures; ++s)
                {
                    const auto& tid = (idata.tex_override_mask & (1u << s))
                                          ? idata.texture_slot_ids[s]
                                          : parent->data()->texture_slot_ids[s];
                    if (!tid.is_nil() && !assets.hasData(tid))
                    { reportMissing(spec, tid); tex_pending = true; }
                }
                if (tex_pending) { spec.pending = true; return spec; }

                spec.instances.push_back(ThumbnailInstanceSpec{sphere_mesh_id, id});
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
        reg.registerProvider(lux::asset::EAssetType::MATERIAL_INSTANCE,
                             std::make_unique<MaterialInstanceThumbnailSpecProvider>());
        return reg;
    }

} // namespace lux::editor
