#pragma once
// Non-installed diagnostic source mutations. No Registry or Session implementation escapes this boundary.
#include <lux/engine/editor/sessions/scene/SceneResourceStatus.hpp>
extern "C" LUX_EDITOR_SCENE_SESSION_PUBLIC void lux_er1_scene_allocation_fail_after(std::size_t) noexcept;
extern "C" LUX_EDITOR_SCENE_SESSION_PUBLIC std::size_t lux_er1_scene_allocation_disarm() noexcept;
namespace lux::editor::sessions::detail
{
    enum class ESceneTestMutation : std::uint8_t
    {
        ADJUST_VISUALS,
        ROTATE_MESH_SOURCES,
        REMOVE_VISUALS,
        CLEAR_SCENE
    };
    struct ResourceAccounting final
    {
        std::size_t requests{}, request_capacity{}, associations{}, association_buckets{};
        std::size_t request_body_bytes{}, request_array_bytes{}, result_body_bytes{}, snapshot_capacity_bytes{};
        std::size_t mesh_payload_capacity_bytes{}, material_spirv_capacity_bytes{};
        std::size_t pending_reads{}, pending_gpu_requests{}, gpu_handles{}, retirement_marker_refs{};
        bool scope_present{}, scope_closed{};
    };
    struct LUX_EDITOR_SCENE_SESSION_PUBLIC SceneTestAccess final
    {
        static const void *pendingEditOperation(const SceneSession &) noexcept;
        static SceneResult<void> setProjectionTransformPresent(SceneSession &, SceneObjectRef, bool) noexcept;
        struct ResourceBackpressure final { std::size_t control{}, upload{}; };
        static SceneResult<ResourceAccounting> resourceAccounting(const SceneSession &) noexcept;
        static ResourceBackpressure resourceBackpressure(bool reset = false) noexcept;
        static SceneResult<void> mutateSource(SceneSession &, ESceneTestMutation) noexcept;
        static SceneResult<SceneEntityRef> recycleSelectedEntity(SceneSession &) noexcept;
        static SceneResult<void> replaceMeshSource(SceneSession &, SceneEntityRef, lux::asset::AssetId) noexcept;
        static bool resourceReadsSettled(const SceneSession &) noexcept;
        static bool resourceReadyForAdoption(const SceneSession &) noexcept;
        static void holdResourceAdoption(bool) noexcept;
        static void failShaderInfoAfterMeshUpload() noexcept;
        static std::optional<ResourceRequestKey> failedShaderKey() noexcept;
        static void rejectMaterialAfterSiblingUploads() noexcept;
        static std::optional<ResourceRequestKey> rejectedMaterialKey() noexcept;
        static std::size_t liveResourceHandles(const SceneSession &, const ResourceRequestKey &) noexcept;
        static SceneResult<std::shared_ptr<const SceneResourceSnapshot>>
        resourceOwnerSnapshot(const SceneSession &) noexcept;
    };
}
