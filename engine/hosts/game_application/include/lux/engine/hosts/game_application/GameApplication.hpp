#pragma once
/**
 * @file GameApplication.hpp
 * @brief Platform-neutral composition root for a compiled game application.
 *
 * A platform adapter supplies a native surface and drives the event loop. This
 * object owns the Runtime closure, scene, render backend and close protocol.
 * The reference Player is one adapter; an independently built game executable
 * can link this installed component and provide its own entry point, resources,
 * metadata and icon without changing the engine.
 */

#include <lux/engine/hosts/game_application/visibility.h>
#include <lux/engine/runtime/extensions/ExtensionModuleManager.hpp>
#include <lux/engine/runtime/render/scene/TextureStreamingBudget.hpp>
#include <lux/engine/runtime/frame/FrameCoordinator.hpp>
#include <lux/engine/navigation/Navigation.hpp>
#include <lux/engine/function/render/Capacity.hpp>
#include <lux/engine/math/Position.hpp>
#include <lux/engine/math/Extent.hpp>
#include <lux/engine/ecs/render/components/3d/DirectionalLightComponent.hpp>
#include <lux/engine/ecs/render/components/3d/HeightFogComponent.hpp>
#include <lux/engine/ecs/render/components/3d/SkyboxComponent.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lux::ecs
{
    class ComponentTypeCatalog;
    struct ScheduleSystemFrameTrace;
}

namespace lux::runtime { class SceneContributionCatalog; }

namespace lux::extensions
{
    class EngineExtensions;
}

namespace lux::input
{
    class ActionMapper;
    class InputActionRegistry;
    class InputContextStack;
}

namespace lux::game
{
    struct GameApplicationCameraPose final
    {
        lux::math::Position3d position{};
        std::array<float, 3u> forward{0.0f, 0.0f, -1.0f};
        std::array<float, 3u> up{0.0f, 1.0f, 0.0f};
    };

    /// Owner-thread automation view of the authored singleton presentation
    /// components. These are ordinary ECS facts; this wrapper only avoids
    /// exposing the SceneRuntime registry through the platform host.
    struct GameApplicationVisualState final
    {
        std::optional<lux::ecs::SkyboxComponent> skybox;
        std::optional<lux::ecs::DirectionalLightComponent>
            directional_light;
        std::optional<lux::ecs::HeightFogComponent> height_fog;
    };

    /// Presence-aware patch used by benchmark/control hosts. Every requested
    /// singleton must already exist and the whole patch is validated before
    /// any registry mutation is published.
    struct GameApplicationVisualPatch final
    {
        std::optional<lux::ecs::SkyboxComponent> skybox;
        std::optional<lux::ecs::DirectionalLightComponent>
            directional_light;
        std::optional<lux::ecs::HeightFogComponent> height_fog;
    };

    /// Owning diagnostic capture of the active main View. Pixels are tightly
    /// packed BGRA8, top row first. This is an explicit automation seam, not a
    /// per-frame gameplay API.
    struct GameApplicationFrameCapture final
    {
        lux::math::Extent2u extent{};
        std::vector<std::uint8_t> pixels_bgra8;
        /// Graph compiled for the temporary offscreen target, captured before
        /// the View is restored to the platform surface.
        std::string render_graph_dump;
    };

    struct GameApplicationHooks final
    {
        /// Called after built-in descriptors are registered and before the
        /// scene is created. Compiled game code may add its own runtime-leaf
        /// contributions without depending on ECS assembly internals.
        std::function<bool(
            lux::runtime::SceneContributionCatalog&,
            lux::ecs::ComponentTypeCatalog&)>
            configure_contributions;

        /// Called before SceneScriptRuntime starts. Compiled game code can
        /// register actions and contexts without modifying a platform shell.
        std::function<bool(
            lux::input::ActionMapper&,
            lux::input::InputActionRegistry&,
            lux::input::InputContextStack&)>
            configure_input;
    };

    struct GameApplicationConfig final
    {
        std::string title = "Lux Game";

        /// Cooked game pak mounted at /Game.
        std::filesystem::path game_pak_file;

        /// Optional cooked engine pak mounted at /Engine.
        std::filesystem::path base_pak_file;

        /// Explicit boot EntityScene vpath. Empty selects the pak's only
        /// EntityScene entry.
        std::string boot_scene;

        /// Platform-owned writable root for durable scene/domain state. The shared
        /// GameApplication never derives this from the read-only deployment.
        std::filesystem::path save_root;

        /// Required native Vulkan instance extensions supplied by the platform.
        std::vector<std::string> vulkan_instance_extensions;

        std::vector<lux::extensions::ExtensionModuleRequirement> extensions;
        GameApplicationHooks hooks;

        std::size_t blocking_io_threads{0};
        std::size_t background_cpu_concurrency{0};
        lux::runtime::TextureStreamingBudget texture_streaming{};
        lux::render::CapacityRequest capacity_request{};
        bool enable_validation{false};
    };

    /// End-of-run diagnostic snapshot. Render-domain fields are obtained with
    /// bounded control RPCs and ECS fields are counted only when this method is
    /// explicitly called; the normal frame path never scans the Registry.
    struct GameApplicationTelemetry final
    {
        std::uint64_t frame_opened{0u};
        std::uint64_t frame_submitted{0u};
        std::uint64_t frame_slot_wait_nanoseconds{0u};
        std::uint64_t frame_slot_wait_max_nanoseconds{0u};
        std::uint32_t validation_error_count{0u};
        bool camera_pose_valid{false};
        double camera_position_x{0.0};
        double camera_position_y{0.0};
        double camera_position_z{0.0};
        float camera_forward_x{0.0f};
        float camera_forward_y{0.0f};
        float camera_forward_z{-1.0f};

        /// EntityScene/Spatial3D residency facts. These replace the old
        /// dimension/topology probe: a 3D streamed scene is identified by its
        /// selected contribution and by the ECS interest adapter which is
        /// actually driving Section demand.
        bool spatial3d_catalog_present{false};
        bool spatial3d_interest_available{false};
        bool spatial3d_camera_interest{false};
        std::uint64_t spatial3d_tracked_sources{0u};
        std::uint64_t spatial3d_active_sections{0u};
        std::uint64_t spatial3d_resident_sections{0u};
        std::uint64_t spatial3d_waiting_sections{0u};
        std::uint64_t spatial3d_staging_sections{0u};
        std::uint64_t spatial3d_published_sections{0u};
        std::uint64_t spatial3d_failed_sections{0u};

        std::uint64_t async_accepted{0u};
        std::uint64_t async_rejected{0u};
        std::uint64_t async_active_operations{0u};
        std::uint64_t async_queue_high_water{0u};
        std::uint64_t async_byte_high_water{0u};
        std::uint64_t async_main_queue_high_water{0u};
        std::uint64_t async_coordinator_handler_nanoseconds{0u};
        std::uint64_t async_coordinator_handler_max_nanoseconds{0u};
        std::uint64_t async_blocking_io_running{0u};
        std::uint64_t async_background_cpu_running{0u};

        std::uint64_t upload_submitted_packets{0u};
        std::uint64_t upload_shared_bytes{0u};
        std::uint64_t upload_copied_bytes{0u};
        std::uint64_t upload_pending_backpressure{0u};
        std::uint64_t upload_active_replies{0u};
        std::uint64_t upload_accepted_inflight{0u};
        std::uint64_t upload_retry_attempts{0u};
        std::uint64_t upload_retry_high_water{0u};
        std::uint64_t upload_queue_high_water{0u};
        std::uint64_t upload_payload_high_water{0u};

        std::uint32_t physics_dynamic_bodies{0u};
        std::uint32_t physics_characters{0u};
        std::uint32_t physics_static_heightfield_bodies{0u};
        std::uint64_t physics_capacity_bytes{0u};
        std::uint32_t physics_allocation_count{0u};
        std::uint64_t navigation_generation{0u};
        std::uint32_t navigation_waiting_regions{0u};
        std::uint32_t navigation_staging_regions{0u};
        std::uint32_t navigation_ready_regions{0u};
        std::uint32_t navigation_active_regions{0u};
        std::uint32_t navigation_retiring_regions{0u};
        std::uint64_t navigation_requests_emitted{0u};
        std::uint64_t navigation_queue_backpressure{0u};
        std::uint64_t navigation_stale_completions{0u};
        std::uint64_t navigation_failed_regions{0u};
        std::uint64_t navigation_staging_work_items{0u};
        std::uint64_t navigation_retirement_work_items{0u};
        std::uint64_t navigation_staging_bytes{0u};
        std::uint64_t navigation_retired_bytes{0u};
        std::uint64_t navigation_close_hides{0u};
        std::uint64_t navigation_owner_bytes{0u};
        std::uint32_t navigation_maximum_staging_work_items_per_tick{0u};
        std::uint32_t navigation_maximum_retirement_work_items_per_tick{0u};
        std::uint32_t navigation_maximum_close_hides_per_tick{0u};
        std::uint64_t navigation_queries_submitted{0u};
        std::uint64_t navigation_queries_completed{0u};
        std::uint64_t navigation_queries_failed{0u};
        std::uint64_t navigation_queries_complete_paths{0u};
        std::uint64_t navigation_queries_partial_paths{0u};
        std::uint64_t navigation_queries_pending_paths{0u};
        std::uint32_t navigation_last_path_points{0u};
        std::uint32_t navigation_last_missing_regions{0u};

        std::uint64_t root_3d_transforms{0u};
        std::uint64_t point_lights{0u};
        std::uint64_t spot_lights{0u};
        std::uint64_t directional_lights{0u};
        std::uint64_t water_surfaces{0u};
        std::uint64_t rigid_bodies_3d{0u};
        std::uint64_t character_controllers_3d{0u};
        /// Main-owner revisions of authored presentation facts. Zero means
        /// that the corresponding singleton has not been observed.
        std::uint64_t sky_revision{0u};
        std::uint64_t directional_light_revision{0u};
        std::uint64_t height_fog_revision{0u};

        bool render_cluster_available{false};
        std::uint32_t render_clusters{0u};
        std::uint32_t render_instances{0u};
        std::uint32_t visible_render_clusters{0u};
        std::uint32_t visible_render_instances{0u};
        std::uint32_t world_classic_render_clusters{0u};
        std::uint32_t world_hlod_render_clusters{0u};
        std::uint32_t world_classic_render_instances{0u};
        std::uint32_t world_hlod_render_instances{0u};
        bool world_render_bounds_valid{false};
        double world_render_min_x{0.0};
        double world_render_min_y{0.0};
        double world_render_min_z{0.0};
        double world_render_max_x{0.0};
        double world_render_max_y{0.0};
        double world_render_max_z{0.0};
        std::uint32_t gpu_render_candidates{0u};
        std::uint32_t gpu_render_candidates_requested{0u};
        std::uint32_t gpu_render_candidates_overflow{0u};
        std::uint32_t gpu_render_candidate_groups{0u};
        bool gpu_render_candidates_valid{false};
        std::uint32_t cull_visible_flag_instances{0u};
        std::uint32_t cull_gbuffer_pass_instances{0u};
        std::uint32_t cull_geometry_instances{0u};
        std::uint32_t cull_lod_instances{0u};
        std::uint32_t cull_mdc_instances{0u};
        std::uint32_t cull_frustum_instances{0u};
        std::uint32_t non_white_render_instances{0u};
        std::uint32_t render_instance_rgba8_xor{0u};
        std::uint32_t wanted_mip_textures{0u};
        std::uint32_t minimum_wanted_mip{0xffffffffu};
        std::uint32_t workgroup_aggregation_fallbacks{0u};
        bool wanted_mip_feedback_valid{false};
        std::uint64_t texture_full_bytes{0u};
        std::uint64_t texture_target_bytes{0u};
        std::uint64_t texture_actual_bytes{0u};
        std::uint64_t render_cluster_cpu_capacity_bytes{0u};
        std::uint32_t render_cluster_cpu_allocation_count{0u};

        bool mesh_stack_available{false};
        std::uint32_t actor_render_instances{0u};
        std::uint32_t actor_transitioning_instances{0u};
        std::uint32_t actor_resource_bound_instances{0u};
        std::uint32_t transparent_actor_hard_cuts{0u};
        std::uint32_t mesh_vbo_segments{0u};
        std::uint32_t mesh_ibo_segments{0u};
        std::uint32_t mesh_vbo_growths{0u};
        std::uint32_t mesh_ibo_growths{0u};
        std::uint64_t mesh_vbo_used_bytes{0u};
        std::uint64_t mesh_vbo_free_bytes{0u};
        std::uint64_t mesh_vbo_largest_free_block{0u};
        std::uint64_t mesh_ibo_used_bytes{0u};
        std::uint64_t mesh_ibo_free_bytes{0u};
        std::uint64_t mesh_ibo_largest_free_block{0u};
        float mesh_vbo_fragmentation{0.0f};
        float mesh_ibo_fragmentation{0.0f};

        bool light_render_available{false};
        std::uint32_t render_directional_lights{0u};
        std::uint32_t render_point_lights{0u};
        std::uint32_t render_spot_lights{0u};
        std::uint32_t render_area_lights{0u};
        std::uint32_t transitioning_lights{0u};

        bool skybox_available{false};
        std::uint32_t skybox_active_mode{0u};
        std::uint32_t skybox_bindless_index{0u};
        std::uint32_t skybox_pass_visits{0u};
        std::uint32_t skybox_draws{0u};
        std::uint32_t skybox_inactive_pass_visits{0u};
        std::uint32_t skybox_pipeline_bind_failures{0u};
        float skybox_intensity{0.0f};

        bool terrain_available{false};
        std::uint32_t terrain_resident_pages{0u};
        std::uint32_t terrain_full_resolution_pages{0u};
        std::uint32_t terrain_fallback_pages{0u};
        std::uint32_t terrain_selected_patches{0u};
        bool terrain_selected_patches_valid{false};
        std::uint32_t terrain_fine_pages{0u};
        std::uint32_t terrain_hlod_pages{0u};
        std::uint32_t terrain_drawable_pages{0u};
        std::uint32_t terrain_drawable_pages_by_level[5]{};
        std::uint32_t terrain_transition_pages{0u};
        bool terrain_view_surface_valid{false};
        std::uint32_t terrain_view_surface_level{0u};
        std::int32_t terrain_view_page_x{0};
        std::int32_t terrain_view_page_y{0};
        std::int32_t terrain_view_page_z{0};
        float terrain_view_local_x{0.0f};
        float terrain_view_local_y{0.0f};
        float terrain_view_local_z{0.0f};
        float terrain_view_surface_height{0.0f};
        float terrain_view_surface_clearance{0.0f};
        std::uint64_t terrain_cpu_bytes{0u};
        std::uint64_t terrain_gpu_bytes{0u};

        bool water_available{false};
        std::uint32_t water_resident_surfaces{0u};
        std::uint32_t water_visible_patches{0u};
        std::uint32_t water_transitioning_surfaces{0u};
        std::uint32_t transparent_hard_cuts{0u};
        std::uint64_t water_cpu_bytes{0u};
        std::uint64_t water_gpu_capacity_bytes{0u};
    };

    class LUX_GAME_APPLICATION_PUBLIC GameApplication final
    {
    public:
        GameApplication();
        ~GameApplication();

        GameApplication(const GameApplication&) = delete;
        GameApplication& operator=(const GameApplication&) = delete;

        /// Transactionally starts the process Runtime and first surface.
        [[nodiscard]] bool start(
            GameApplicationConfig config,
            std::uint64_t native_surface,
            lux::math::Extent2u extent);

        /// Attach a newly acquired platform surface. If the scene is already
        /// live, its present view is rebound at the render safe point.
        [[nodiscard]] bool attachSurface(
            std::uint64_t native_surface,
            lux::math::Extent2u extent);

        /// Complete the two-phase target release before the platform invalidates
        /// its native window. Safe when no surface is attached.
        [[nodiscard]] bool detachSurface() noexcept;

        /// Advance one playable frame. dt is clamped by the shared runtime rule.
        [[nodiscard]] bool tick(float dt, lux::math::Extent2u extent);

        [[nodiscard]] bool sceneReady() const noexcept;

        /// Pump replies/completions/events without opening a render frame.
        std::size_t pumpSafePoint();

        /// Pump once, then wait on the shared progress epoch for at most max_wait.
        std::size_t pumpIdleFor(std::chrono::steady_clock::duration max_wait);

        /// Installs the platform event-loop wake. The application also wakes
        /// render request settlement, so adapters only wake their native loop.
        void bindExternalWake(std::function<void()> wake);
        void unbindExternalWake() noexcept;

        [[nodiscard]] std::optional<std::string> renderGraphDump();
        [[nodiscard]] std::optional<std::string> gpuTimingDump();

        [[nodiscard]] std::optional<lux::runtime::FrameTrace>
        latestFrameTrace() const noexcept;
        [[nodiscard]] std::span<
            const lux::ecs::ScheduleSystemFrameTrace>
        latestScheduleSystemFrameTrace() const noexcept;
        [[nodiscard]] std::vector<lux::runtime::FrameTrace>
        frameTraceHistory() const;

        [[nodiscard]] const lux::render::CapacityPlan&
        capacityPlan() const noexcept;
        [[nodiscard]] const std::optional<lux::render::CapacityShortfall>&
        capacityShortfall() const noexcept;

        /// Owner-main-thread automation seam. Updates the active camera's
        /// absolute position and orientation through EnTT patch notifications.
        [[nodiscard]] bool setMainCameraPose(
            const GameApplicationCameraPose& pose) noexcept;

        /// Updates the active perspective camera's finite clip range through
        /// EnTT patch notification. Product hosts keep the ordinary 0.1/1000
        /// defaults; large-world tools may opt into a range matching their
        /// streaming profile without coupling Camera3D to World Partition.
        [[nodiscard]] bool setMainCameraClipRange(
            float near_z,
            float far_z) noexcept;

        [[nodiscard]] bool patchVisualState(
            const GameApplicationVisualPatch& patch) noexcept;

        [[nodiscard]] std::optional<GameApplicationVisualState>
        visualState() const noexcept;

        /// Executes one owner-thread navigation query against the scene's
        /// backend-independent query service. Missing content is reported as
        /// PENDING with concrete region identities.
        [[nodiscard]] std::optional<lux::navigation::NavigationPathResult>
        queryNavigationPath(
            const lux::navigation::NavigationPathRequest& request) noexcept;

        /// Captures explicit benchmark/diagnostic telemetry. This is not a
        /// per-frame API and may issue bounded render-control queries.
        [[nodiscard]] std::optional<GameApplicationTelemetry>
        telemetrySnapshot();

        /// Temporarily routes the active main View to an offscreen target,
        /// renders a bounded number of stationary settle frames, reads back
        /// BGRA8 pixels, then restores the platform surface. Main-owner only.
        [[nodiscard]] std::optional<GameApplicationFrameCapture>
        captureMainView(std::uint32_t settle_frames = 4u);

        /// Executes the complete active close protocol. A watchdog failure
        /// returns false and keeps ownership intact for diagnostics/retry.
        [[nodiscard]] bool close() noexcept;

        [[nodiscard]] bool live() const noexcept;
        [[nodiscard]] bool surfaceAttached() const noexcept;

        [[nodiscard]] lux::input::ActionMapper& inputMapper() noexcept;
        [[nodiscard]] lux::input::InputActionRegistry&
            inputActions() noexcept;
        [[nodiscard]] lux::input::InputContextStack& inputContexts() noexcept;
        [[nodiscard]] lux::extensions::EngineExtensions& extensions() noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
