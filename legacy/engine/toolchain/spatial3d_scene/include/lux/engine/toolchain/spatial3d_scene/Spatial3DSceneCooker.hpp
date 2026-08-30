#pragma once
/**
 * @file Spatial3DSceneCooker.hpp
 * @brief Toolchain-only LXWA v5 to SceneDescription/LXES cooker for 3D data.
 *
 * The input is the owning, page-free Spatial3D Authoring model. The result
 * contains exclusively SceneDescription records and domain-owned content blobs;
 * no Runtime target needs a legacy cooked-World reader for this path.
 */

#include <lux/engine/toolchain/entity_scene/EntitySceneCooker.hpp>
#include <lux/engine/toolchain/spatial3d_scene/Spatial3DAuthoringSource.hpp>
#include <lux/engine/toolchain/spatial3d_scene/visibility.h>

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/math/Position.hpp>
#include <lux/engine/ecs/scene_format/spatial3d/SceneCatalog.hpp>
#include <lux/engine/ecs/spatial3d/streaming/Spatial3DStreamingPolicy.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lux::toolchain
{
    /// Immutable Runtime Mesh images made available to the Spatial3D cook.
    /// The cooker validates the embedded asset identity and decodes only the
    /// meshes actually referenced by a visual-HLOD node.  Keeping this as an
    /// owning value makes the Toolchain boundary independent of AssetManager,
    /// VFS mounts and caller thread lifetime.
    struct Spatial3DMeshAssetSource final
    {
        lux::asset::asset_id_t id{};
        std::vector<std::byte> encoded_image;
    };

    struct Spatial3DMeshAssetCatalog final
    {
        std::vector<Spatial3DMeshAssetSource> meshes;
    };

    /// One domain-owned Mesh asset generated for a coarse visual-LOD node.
    /// `virtual_path` is mount-relative and extensionless; composition roots
    /// publish the exact encoded image into the same Pak as LXSC/LXES.
    struct CookedSpatial3DMeshAsset final
    {
        lux::asset::asset_id_t id{};
        std::string virtual_path;
        std::vector<std::byte> encoded_image;
        std::uint32_t source_instance_count{0u};
        std::uint32_t source_vertex_count{0u};
        std::uint32_t source_triangle_count{0u};
        std::uint32_t output_vertex_count{0u};
        std::uint32_t output_triangle_count{0u};
        float simplification_error{0.0f};
    };

    /// Spatial3D-specific cook result.  The generic EntityScene bundle stays
    /// domain blind; generated geometry belongs to this Toolchain leaf.
    struct CookedSpatial3DEntitySceneBundle final
        : CookedSceneDescriptionBundle
    {
        std::vector<CookedSpatial3DMeshAsset> generated_meshes;
    };

    /// Publication policy for fixed startup facts and independently resident
    /// fine/visual-LOD spatial Sections.
    struct Spatial3DSceneCookerConfig final
    {
        /// Empty preserves the Authoring scene UUID in the cooked Scene.
        lux::asset::asset_id_t scene_id;
        /// Absolute, extensionless Pak directory for emitted LXES objects.
        std::string section_content_prefix{"/Game/EntitySections"};
        lux::math::Position3d fallback_camera_position{
            512.0, 256.0, 512.0};
        /// 3D selector policy belongs to this leaf cooker configuration, not
        /// to the dimension-neutral LXWA root.
        double fine_active_distance{2048.0};
        double fine_resident_distance{2560.0};
        std::uint32_t fine_active_priority{150u};
        float visual_lod_enter_error_pixels{2.5f};
        float visual_lod_exit_error_pixels{1.5f};
        /// Per-level best-effort triangle target. Level 1 targets this ratio,
        /// level N targets ratio^N. Authored source LODs are consumed first;
        /// a topologically irreducible group remains a generated merged Mesh
        /// instead of making otherwise valid scene content unloadable.
        float visual_lod_triangle_ratio{0.5f};
        /// meshoptimizer relative-error ceiling for each generated mesh.
        float visual_lod_max_simplification_error{0.05f};
        /// Offline-work bounds for one visual-LOD node. A cook fails before
        /// growing the merge buffers beyond these values; it never silently
        /// falls back to the old unsimplified instance aggregation path.
        std::uint32_t visual_lod_max_source_instances{1'000'000u};
        std::uint32_t visual_lod_max_generated_meshes{4'096u};
        std::uint32_t visual_lod_max_merged_vertices{2'000'000u};
        std::uint32_t visual_lod_max_merged_indices{6'000'000u};
        double visual_lod_active_scale{4.0};
        double visual_lod_resident_scale{4.0};
        /// Cooked, fixed resident-set admission.  It does not scale with the
        /// number of distant catalog entries.
        lux::ecs::spatial3d::streaming::ResidencyCapacity residency;
    };

    enum class ESpatial3DSceneCookError : std::uint8_t
    {
        INVALID_ARGUMENT,
        INVALID_POSITION,
        INVALID_ACTOR,
        MISSING_COMPONENT_SCHEMA,
        COMPONENT_SCHEMA_MISMATCH,
        INVALID_COMPONENT_PAYLOAD,
        CLASSIC_MESH_CONTENT_REJECTED,
        TERRAIN_CONTENT_REJECTED,
        PHYSICS_CONTENT_REJECTED,
        NAVIGATION_CONTENT_REJECTED,
        SPATIAL_CATALOG_REJECTED,
        ENTITY_SCENE_COOK_REJECTED,
        AUTHORING_SOURCE_REJECTED
    };

    struct Spatial3DSceneCookFailure final
    {
        ESpatial3DSceneCookError code{
            ESpatial3DSceneCookError::INVALID_ARGUMENT};
        std::string detail;
        std::optional<EntitySceneCookFailure> entity_scene;
        std::optional<std::string> authoring_source;
    };

    /// Converts an already-loaded, owning 3D Authoring source. Component
    /// descriptors are supplied by the Toolchain composition root so sparse
    /// tagged payloads can be materialized with current defaults and
    /// re-emitted as exact LXES component columns.  This also performs the
    /// mandatory private-NameTable to Section-NameTable remap.
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_SPATIAL3D_SCENE_PUBLIC
    lux::cxx::expected<
        CookedSpatial3DEntitySceneBundle,
        Spatial3DSceneCookFailure>
    cookSpatial3DEntityScene(
        const Spatial3DAuthoringSource& source,
        const lux::ecs::ComponentTypeCatalog& components,
        const Spatial3DMeshAssetCatalog& mesh_assets,
        Spatial3DSceneCookerConfig config = {});

    /// Blocking Toolchain convenience: read the LXWA v5 object graph into the
    /// owning 3D model, then immediately cross the one-way boundary above.
    /// Runtime composition must never call this overload.
    [[nodiscard]] LUX_ENGINE_TOOLCHAIN_SPATIAL3D_SCENE_PUBLIC
    lux::cxx::expected<
        CookedSpatial3DEntitySceneBundle,
        Spatial3DSceneCookFailure>
    cookSpatial3DEntitySceneSource(
        const std::filesystem::path& root_document,
        const lux::ecs::ComponentTypeCatalog& components,
        const Spatial3DMeshAssetCatalog& mesh_assets,
        Spatial3DSceneCookerConfig config = {},
        const Spatial3DAuthoringLoadLimits& limits = {});
} // namespace lux::toolchain
