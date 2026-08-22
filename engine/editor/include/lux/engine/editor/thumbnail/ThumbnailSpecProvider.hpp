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
 *   - MESH           → one instance (the mesh asset id + no material).
 *   - MODEL          → N instances (the model's sub-mesh asset ids + their
 *                      resolved material ids), merged bounds.
 *   - MATERIAL       → one instance: the built-in sphere id wearing this
 *                      material id.
 *
 * Providers are SYNCHRONOUS and cheap: they only fetch asset data and prepare
 * CPU pixels / asset-id references + bounds. They do NOT touch the
 * RenderFrameSession, the GPU, or any scene. `ThumbnailService` executes the spec
 * asynchronously — it spawns job ENTITIES (MeshComponent{ids}) in its own
 * SceneRuntime, waits for the render subsystems to report the instances ready
 * (`MeshInstanceReadyComponent`), then reads the offscreen target back. The
 * spec therefore speaks ASSET IDS, not GPU handles or mesh-data pointers —
 * upload/refcount/teardown all ride the standard resource-resolver path.
 */

#include <lux/engine/resource/asset/Asset.hpp>          // asset_id_t, EAssetType
#include <lux/engine/math/AABB.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lux::asset { class AssetManager; }

namespace lux::editor
{
    /// A requested output resolution (thumbnails are typically square).
    struct ThumbnailSize
    {
        std::uint32_t width{0};
        std::uint32_t height{0};
    };

    /// One mesh + material to render, by ASSET ID (the service turns each into
    /// a job entity carrying a MeshComponent; the resource resolver does the
    /// upload/refcount). `material_asset_id` nil means "no authored material" —
    /// the service substitutes the built-in PreviewGrey material so bare meshes
    /// keep the familiar neutral-grey look.
    ///
    /// Skinned meshes render the BIND POSE: the unified vertex layout stores
    /// bind-pose positions and the job entity is a plain (static) MeshComponent,
    /// so a static draw IS the bind pose — same behaviour the retired hand-
    /// rolled capture path had.
    struct ThumbnailInstanceSpec
    {
        lux::asset::asset_id_t mesh_asset_id{};       // never nil in a valid spec
        lux::asset::asset_id_t material_asset_id{};   // nil => PreviewGrey
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

        /// Missing dependencies reported by the pure provider. The service
        /// owns request de-duplication and all AssetClient interaction.
        std::vector<lux::asset::asset_id_t> missing_assets;

        // Texture path: ready RGBA8 pixels (no scene render needed).
        bool                   has_cpu_pixels{false};
        std::vector<std::byte> rgba8;
        std::uint32_t          cpu_width{0};
        std::uint32_t          cpu_height{0};

        // 3D path: instances to spawn in the service's preview SceneRuntime +
        // framing bounds.
        std::vector<ThumbnailInstanceSpec> instances;
        lux::math::AABB                    bounds;
    };

    class IThumbnailSpecProvider
    {
    public:
        virtual ~IThumbnailSpecProvider() = default;

        /// Build the CPU spec for @p asset_id. Synchronous + cheap (asset fetch +
        /// CPU pixel prep / id resolution only — NO RenderFrameSession, NO scene).
        /// @p sphere_mesh_id is the built-in preview-sphere mesh asset (the
        /// material-ball geometry) — the ONE piece of preview knowledge a
        /// provider needs; nil when the builtin failed to register (a material
        /// spec then reports invalid). Returns `{valid=false}` on failure /
        /// unsupported asset; `{valid=false, pending=true}` when a needed asset
        /// is a data-less shell (retry later). The returned spec lists every
        /// missing dependency; providers never perform loading themselves.
        [[nodiscard]] virtual ThumbnailSpec buildSpec(
            lux::asset::AssetManager&     assets,
            const lux::asset::asset_id_t& sphere_mesh_id,
            const lux::asset::asset_id_t& asset_id) = 0;
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
