#pragma once
/**
 * @file ThumbnailSpecProvider.hpp
 * @brief Per-asset-type thumbnail *spec providers* + a type→provider registry.
 *
 * Echoes the editor's `AssetTypeRegistry` pattern (UE's analogue is
 * `UThumbnailRenderer`, but the name there is a misnomer — these never render).
 * A provider turns one asset into a CPU-side `ThumbnailSpec` — the *recipe* for
 * its thumbnail:
 *   - TEXTURE        → ready RGBA8 pixels (no scene render).
 *   - MESH           → one instance (the mesh + a neutral material).
 *   - MODEL          → N instances (the model's meshes), merged bounds.
 *   - MATERIAL → one instance: the built-in sphere wearing the graph material.
 *
 * Providers are SYNCHRONOUS and cheap: they only fetch asset data and prepare
 * CPU pixels / geometry references. They do NOT touch the RenderSession or the
 * GPU. `ThumbnailService` executes the spec asynchronously (upload → instance →
 * deferred readback → encode → display upload) across frames, so the UI thread
 * never blocks. This separation is what makes non-blocking generation possible.
 */

#include <lux/engine/asset/Asset.hpp>          // asset_id_t, EAssetType
#include <lux/engine/math/AABB.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lux::rdesc { class Mesh; }
namespace lux::asset { class AssetManager; }

namespace lux::editor
{
    class PreviewScene;

    /// "请异步加载这个资产的数据(它已注册为空壳)"。幂等(执行器按 id 去重)。
    /// 大世界 W2b:启动改注册 info 空壳后,浏览器里从未入场景的资产数据也得有人触发
    /// 加载,否则缩略图永远空白 —— provider 在缺数据时用它请求加载并报 pending。
    using ThumbnailLoadFn = std::function<void(const lux::asset::asset_id_t&)>;

    /// A requested output resolution (thumbnails are typically square).
    struct ThumbnailSize
    {
        std::uint32_t width{0};
        std::uint32_t height{0};
    };

    /// One mesh+material to render in the preview. `mesh_data == nullptr` means
    /// "use the PreviewScene's resident sphere"; `graph_material_id` nil means
    /// "use the PreviewScene's resident default (grey graph) material". `mesh_data`
    /// borrows from the asset (kept alive by the AssetManager for the job's lifetime).
    ///
    /// `graph_material_id` is a MATERIAL asset UUID. The service loads the
    /// baked asset (gbuffer/forward SPIR-V + params + texture-slot UUIDs), compiles
    /// the frags, resolves each texture slot to a bindless index, and uploads it via
    /// uploadGraphMaterial — so the sphere wears the real graph material (its own PSO,
    /// exactly like a scene mesh). W5 retired rdesc::Material: every material the
    /// editor renders is a graph material now.
    struct ThumbnailInstanceSpec
    {
        const lux::rdesc::Mesh* mesh_data{nullptr};
        lux::asset::asset_id_t  graph_material_id{};   // nil => default grey material

        /// Skinned meshes must be INSTANCED as SkinnedMesh (the draw selects
        /// the skinned pipeline variant) and given a bone palette — the
        /// thumbnail renders the BIND POSE via identity matrices. Instancing
        /// them as static is undefined (the source vertices feed a skinning
        /// path that never ran).
        bool          skinned{false};
        std::uint32_t bone_count{0};   ///< identity palette size when skinned
    };

    /// CPU-side recipe for an asset's thumbnail, produced synchronously by a
    /// provider and executed asynchronously by the service.
    struct ThumbnailSpec
    {
        bool valid{false};

        /// Deps still streaming in (the asset or one of its dependencies is a
        /// data-less shell whose load was just requested). The service should
        /// RETRY this job later rather than mark it permanently Failed. Only
        /// meaningful when `valid == false`.
        bool pending{false};

        // Texture path: ready RGBA8 pixels (no scene render needed).
        bool                   has_cpu_pixels{false};
        std::vector<std::byte> rgba8;
        std::uint32_t          cpu_width{0};
        std::uint32_t          cpu_height{0};

        // 3D path: instances to render in the PreviewScene + framing bounds.
        std::vector<ThumbnailInstanceSpec> instances;
        lux::math::AABB                    bounds;
    };

    class IThumbnailSpecProvider
    {
    public:
        virtual ~IThumbnailSpecProvider() = default;

        /// Build the CPU spec for @p asset_id. Synchronous + cheap (asset fetch +
        /// CPU pixel/geometry prep only — NO RenderSession). Returns
        /// `{valid=false}` on failure / unsupported asset; `{valid=false,
        /// pending=true}` when a needed asset is a data-less shell whose load
        /// was just requested via @p request_load (retry later). @p request_load
        /// is idempotent.
        [[nodiscard]] virtual ThumbnailSpec buildSpec(
            lux::asset::AssetManager&     assets,
            const PreviewScene&           preview,
            const lux::asset::asset_id_t& asset_id,
            const ThumbnailLoadFn&        request_load) = 0;
    };

    /// Maps `EAssetType` → provider. Owns the provider instances.
    class ThumbnailSpecProviderRegistry
    {
    public:
        void registerProvider(lux::asset::EAssetType type,
                              std::unique_ptr<IThumbnailSpecProvider> provider);

        [[nodiscard]] IThumbnailSpecProvider* get(lux::asset::EAssetType type) const noexcept;

    private:
        std::unordered_map<int, std::unique_ptr<IThumbnailSpecProvider>> providers_;
    };

    /// A registry pre-populated with the built-in spec providers
    /// (texture / mesh / model / graph material).
    [[nodiscard]] ThumbnailSpecProviderRegistry makeDefaultThumbnailSpecProviders();

} // namespace lux::editor
