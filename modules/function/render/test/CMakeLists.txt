set(SHADER_COMPILE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/gen/shaders)
set(SHADER_CONFIG_OUT_PATH     ${CMAKE_BINARY_DIR}/gen/test/function/render/include)
set(RENDER_PINCLUDE_DIR        ${CMAKE_CURRENT_SOURCE_DIR}/../pinclude)
set(RENDER_FEATURE_PINCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../features/pinclude)

find_package(imgui REQUIRED COMPONENTS glfw3 vulkan)

component_generate_runtime_asset_path_header(
    COMPONENT render_features
    OUTPUT_HEADER ${SHADER_CONFIG_OUT_PATH}/test_time_asset_path.hpp
)

# ─────────────────────────────────────────────────────────────────────────────
#  Test policy (2026-07-06): functional-verification tests are DEVELOPMENT-TIME
#  artifacts — they gate a change while it is being built (incl. mutation
#  verification) and are deleted once the change has landed green. What stays
#  in-tree permanently is only what has value at every future run:
#    - stress / performance targets (throughput, load, frame-time), and
#    - interactive visual demos (eyeballing a pipeline on screen).
#  Shared bring-up infrastructure (DeviceRenderFixture.hpp) also stays — it is a
#  tool for the NEXT slice's development-time tests, not a test itself.
#
#  Relocated (engine tier, NOT built here): game_loop_test / deferred_stress_test /
#  skeletal_mesh_walk_test / two_scene_shadow_isolation_test /
#  deferred_multi_shading_model_test live in this directory but are built by
#  engine/toolchain/shader/glsl/test (W5a: they need the graph-material cook backend).
#
#  The disabled skeletal_world_demo was deleted: it encoded the retired
#  RenderSession/flat-RenderSystem/material path and was not a compilable
#  compatibility contract.
# ─────────────────────────────────────────────────────────────────────────────

add_executable(
	point_cloud_stress_test
	point_cloud_stress_test.cpp
)

target_link_libraries(
	point_cloud_stress_test
	PRIVATE
	render_features
	lux::engine::platform::window
	lux::engine::resource::asset
)

target_include_directories(
	point_cloud_stress_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

add_executable(
	point_cloud_transient_test
	point_cloud_transient_test.cpp
)

target_link_libraries(
	point_cloud_transient_test
	PRIVATE
	render_features
	lux::engine::platform::window
	lux::engine::resource::asset
)

target_include_directories(
	point_cloud_transient_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

add_executable(
	trajectory_line_test
	trajectory_line_test.cpp
)

target_link_libraries(
	trajectory_line_test
	PRIVATE
	render_features
	lux::engine::platform::window
	lux::engine::resource::asset
)

target_include_directories(
	trajectory_line_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

add_executable(
	gizmo_transient_test
	gizmo_transient_test.cpp
)

target_link_libraries(
	gizmo_transient_test
	PRIVATE
	render_features
	lux::engine::platform::window
	lux::engine::resource::asset
)

target_include_directories(
	gizmo_transient_test
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

# spatial_cull_grid_stress_test — headless GPU proof of the W1 coarse-cull payoff
# under load: a ~25k-instance grid, camera recedes -> activeInstanceCount is
# non-increasing and falls to 0; reads back the GPU active-mask buffer.
add_executable(
    spatial_cull_grid_stress_test
    spatial_cull_grid_stress_test.cpp
)

target_link_libraries(
    spatial_cull_grid_stress_test
    PRIVATE
    render_features
    lux::engine::platform::window
)

target_include_directories(
    spatial_cull_grid_stress_test
    PRIVATE
    ${RENDER_PINCLUDE_DIR}
    ${SHADER_CONFIG_OUT_PATH}
)

# PERMANENT (gpu stress): GPU-driven Canvas2D v2 under load — bulk creation,
# static frames (zero client commands), bulk transform deltas, key-churn order
# rebuilds, remove/create storms. Prints ms/frame for the implementation log.
add_executable(canvas2d_stress_test canvas2d_stress_test.cpp)
target_link_libraries(canvas2d_stress_test PRIVATE render_features lux::engine::platform::window)
target_include_directories(canvas2d_stress_test PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

# PERMANENT (gpu stress): the scene-LIFECYCLE gate (editor architecture v2 C1) —
# createScene(editor 17-feature set) → frames → destroyScene ×100 with the
# validation layer asserted to zero errors (incl. destroy-time leak reports).
# Regression net for the editor's per-scene create/destroy model (C2/C3) and
# the mechanised half of the feature-detach residue audit (C12).
add_executable(scene_cycle_stress_test scene_cycle_stress_test.cpp)
target_link_libraries(scene_cycle_stress_test PRIVATE render_features lux::engine::platform::window)
target_include_directories(scene_cycle_stress_test PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

# PERMANENT (gpu): an owning UploadChannel packet must complete while the frame
# producer is paused. The test deliberately never publishes a FrameProgram and
# drops the caller's source bytes immediately after admission.
add_executable(upload_without_frame_test upload_without_frame_test.cpp)
target_link_libraries(upload_without_frame_test PRIVATE render_features lux::engine::platform::window)
target_include_directories(upload_without_frame_test PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

add_executable(
    render_cluster_upload_gpu_probe
    render_cluster_upload_gpu_probe.cpp
)

set(
    RENDER_3D_SEMANTIC_FRAGMENT_SPV
    ${CMAKE_CURRENT_BINARY_DIR}/render_3d_semantic.frag.spv
)
set(
    RENDER_3D_SEMANTIC_GBUFFER_SPV
    ${CMAKE_CURRENT_BINARY_DIR}/render_3d_semantic_gbuffer.frag.spv
)
add_custom_command(
    OUTPUT ${RENDER_3D_SEMANTIC_FRAGMENT_SPV}
    COMMAND ${Vulkan_GLSLC_EXECUTABLE}
        --target-env=vulkan1.2
        -fshader-stage=frag
        ${CMAKE_CURRENT_SOURCE_DIR}/render_3d_semantic.frag
        -o ${RENDER_3D_SEMANTIC_FRAGMENT_SPV}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/render_3d_semantic.frag
    COMMENT "Compiling deterministic 3D semantic fragment shader"
    VERBATIM
)
add_custom_command(
    OUTPUT ${RENDER_3D_SEMANTIC_GBUFFER_SPV}
    COMMAND ${Vulkan_GLSLC_EXECUTABLE}
        --target-env=vulkan1.2
        -fshader-stage=frag
        ${CMAKE_CURRENT_SOURCE_DIR}/render_3d_semantic_gbuffer.frag
        -o ${RENDER_3D_SEMANTIC_GBUFFER_SPV}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/render_3d_semantic_gbuffer.frag
    COMMENT "Compiling deterministic 3D GBuffer semantic fragment shader"
    VERBATIM
)
add_custom_target(
    render_3d_semantic_shader
    DEPENDS
        ${RENDER_3D_SEMANTIC_FRAGMENT_SPV}
        ${RENDER_3D_SEMANTIC_GBUFFER_SPV}
)
add_dependencies(
    render_cluster_upload_gpu_probe
    render_3d_semantic_shader
)
target_link_libraries(
    render_cluster_upload_gpu_probe
    PRIVATE render_features lux::engine::platform::window
)
target_compile_definitions(
    render_cluster_upload_gpu_probe
    PRIVATE
        LUX_RENDER_3D_SEMANTIC_FRAGMENT_SPV="${RENDER_3D_SEMANTIC_FRAGMENT_SPV}"
        LUX_RENDER_3D_SEMANTIC_GBUFFER_SPV="${RENDER_3D_SEMANTIC_GBUFFER_SPV}"
)
target_include_directories(
    render_cluster_upload_gpu_probe
    PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH}
)

add_executable(terrain_gpu_probe terrain_gpu_probe.cpp)
target_link_libraries(
    terrain_gpu_probe
    PRIVATE render_features lux::engine::platform::window
)

add_executable(gpu_timing_probe gpu_timing_probe.cpp)
target_link_libraries(
    gpu_timing_probe
    PRIVATE render_features lux::engine::platform::window
)
target_include_directories(
    gpu_timing_probe
    PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH}
)
target_include_directories(
    terrain_gpu_probe
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../features/sinclude
        ${RENDER_PINCLUDE_DIR}
        ${SHADER_CONFIG_OUT_PATH}
)

# PERMANENT (cpu): owning UploadChannel admission, byte budget, queue
# backpressure, malformed-packet rollback, stop and caller-lifetime independence.
add_executable(upload_channel_contract_test upload_channel_contract_test.cpp)
target_link_libraries(upload_channel_contract_test PRIVATE render_features)
target_include_directories(upload_channel_contract_test PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

# PERMANENT (cpu): SpirvPatcher -- rewrites descriptor locations inside SPIR-V
# according to a LayoutPlan. Pure CPU, no device opened, ~0.1s. The test
# actually moves locations around (production currently uses an identity
# allocation, so feeding it an identity relocation would verify nothing), and
# does a real round trip (relocate / relocate-back) on real .spv files from
# the build tree, verifying byte-for-byte that only literals change and the
# structure is left untouched.
add_executable(spirv_patcher_test spirv_patcher_test.cpp)
target_link_libraries(spirv_patcher_test PRIVATE render_features)
target_include_directories(spirv_patcher_test PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

add_executable(shadow_atlas_placement_test shadow_atlas_placement_test.cpp)
target_link_libraries(shadow_atlas_placement_test PRIVATE render_features)
target_include_directories(
    shadow_atlas_placement_test
    PRIVATE
        ${RENDER_FEATURE_PINCLUDE_DIR}
        ${SHADER_CONFIG_OUT_PATH}
)

# PERMANENT (cpu): every C++ GPU mirror struct is laid out the way the shader's
# copy of it is. Entirely static_assert — building it IS the test.
add_executable(gpu_layout_test gpu_layout_test.cpp)
target_link_libraries(gpu_layout_test PRIVATE render_features)
target_include_directories(gpu_layout_test PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

add_executable(
    texture_sampling_representation_catalog_test
    texture_sampling_representation_catalog_test.cpp
)
target_link_libraries(
    texture_sampling_representation_catalog_test
    PRIVATE render_features
)

add_executable(render_cluster_registry_test render_cluster_registry_test.cpp)
target_link_libraries(render_cluster_registry_test PRIVATE render_features)
target_include_directories(
    render_cluster_registry_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../features/sinclude
        ${SHADER_CONFIG_OUT_PATH}
)

add_executable(terrain_page_cache_test terrain_page_cache_test.cpp)
target_link_libraries(terrain_page_cache_test PRIVATE render_features)
target_include_directories(
    terrain_page_cache_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../features/sinclude
        ${SHADER_CONFIG_OUT_PATH}
)

# PERMANENT (cpu):特性等级协商的判据表 —— 审计 R11 指出这套机制零使用者、
# 全是从未执行过的代码,第一个挂 profile 的特性会踩没人走过的路。本测试是替身。
add_executable(feature_level_test feature_level_test.cpp)
target_link_libraries(feature_level_test PRIVATE render_features)
target_include_directories(feature_level_test PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

# PERMANENT (cpu): dynamic feature registration is counted, rejects removal
# while an instance is live, and keeps module code resident through the final
# dispatcher-unbind window.
add_executable(feature_type_registry_lifetime_test feature_type_registry_lifetime_test.cpp)
target_link_libraries(feature_type_registry_lifetime_test PRIVATE render_features)
target_include_directories(feature_type_registry_lifetime_test PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

# PERMANENT (cpu):RGBuilder 的条件链作用域 —— 链的成员资格是结构保证,
# 不是六处 setCondition 的约定。纯描述数据,无设备。
add_executable(material_instance_bucket_test material_instance_bucket_test.cpp)
target_link_libraries(material_instance_bucket_test PRIVATE render_features)
target_include_directories(material_instance_bucket_test PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

add_executable(condition_chain_scope_test condition_chain_scope_test.cpp)
target_link_libraries(condition_chain_scope_test PRIVATE render_features)
target_include_directories(condition_chain_scope_test PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

# PERMANENT (cpu): 错误类型注册表的幂等性、代化失效与实参展开 —— 三条都是「结构上
# 不可能出错」型的保证,失效时静默,没有测试就没有任何信号。
add_executable(error_registry_test error_registry_test.cpp)
target_link_libraries(error_registry_test PRIVATE render_features)
target_include_directories(error_registry_test PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

# PERMANENT (cpu): FrameDriver creation is an all-or-nothing transaction.
# Deterministic Vulkan function-pointer fakes inject failure at every resource
# boundary and prove that the local candidate rolls prior handles back.
add_executable(frame_driver_factory_test frame_driver_factory_test.cpp)
target_link_libraries(frame_driver_factory_test PRIVATE render_features)
target_include_directories(frame_driver_factory_test PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

add_executable(frame_retirement_plan_test frame_retirement_plan_test.cpp)
target_link_libraries(frame_retirement_plan_test PRIVATE render_features)
target_include_directories(
    frame_retirement_plan_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/pinclude
        ${RENDER_PINCLUDE_DIR}
        ${SHADER_CONFIG_OUT_PATH}
)

add_executable(stable_record_pages_test stable_record_pages_test.cpp)
target_link_libraries(stable_record_pages_test PRIVATE render_features)
target_include_directories(
    stable_record_pages_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/sinclude
)

add_executable(instance_sparse_contract_test instance_sparse_contract_test.cpp)
target_link_libraries(instance_sparse_contract_test PRIVATE render_features)
target_include_directories(
    instance_sparse_contract_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/sinclude
)

add_executable(
    classic_mesh_segment_contract_test
    classic_mesh_segment_contract_test.cpp
)
target_link_libraries(classic_mesh_segment_contract_test PRIVATE render_features)
target_include_directories(
    classic_mesh_segment_contract_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/sinclude
)

# PERMANENT (cpu): swapchain creation/query/image-view construction is a typed,
# exception-free transaction. Vulkan function-pointer fakes preserve exact
# VkResult values and prove rollback of every successfully-created prefix.
add_executable(swapchain_factory_test swapchain_factory_test.cpp)
target_link_libraries(swapchain_factory_test PRIVATE render_features)
target_include_directories(swapchain_factory_test PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

# Full-pack demos are reflection hosts: the sidecar DLLs must be present in
# the executable's import table before meta_module_init() drains their
# registrars. Keep the shared 2D domain set in one place; callers append only
# the extra domains their authored scene actually uses.
function(lux_render_link_2d_pack_sidecars target)
    # Reflection-free builds deliberately do not create component sidecars.
    # Keep the visual hosts buildable in that supported configuration; their
    # catalogue diagnostics will then accurately describe an empty registry.
    if(NOT LUX_BUILD_COMPONENT_META)
        return()
    endif()

    set(_sidecars
        ecs_meta
        render_components_meta
        render_components_2d_meta
        tilemap_meta
        pixel_meta
        animation_2d_meta
        ${ARGN}
    )

    # The function layer is configured before ecs/, where these targets are
    # declared. Do not guard this with if(TARGET): when component metadata is
    # enabled CMake resolves the plain target names after the complete project
    # graph has been configured and should fail loudly if one is absent.
    target_link_libraries(${target} PRIVATE ${_sidecars})
endfunction()

# VISUAL FULL-STACK stress — a living pixel world ON SCREEN: real gameplay World
# (fixed-step CA: raining sand, dripping water, digs) + ~700 images interleaved
# with the field on ONE priority axis (cross-kind pops) + breathing camera, all
# through the real bridges and wire. The Slice C showcase.
add_executable(pixel_world_visual_stress pixel_world_visual_stress.cpp)
target_link_libraries(pixel_world_visual_stress PRIVATE render_features runtime_render_scene_test_support lux::engine::runtime::runtime_execution lux::engine::runtime::runtime_pack_presentation2d lux::engine::resource::asset lux::engine::platform::window)
target_include_directories(pixel_world_visual_stress PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})
lux_render_link_2d_pack_sidecars(pixel_world_visual_stress)

# VISUAL stress — the A2-02 tilemap path under load: a 32k-tile map (ONE canvas
# instance + R16 index texture), auto-scrolling camera, per-frame mining-wave
# edits (dirty-rect region uploads) + periodic full regeneration.
add_executable(tilemap_visual_stress tilemap_visual_stress.cpp)
target_link_libraries(tilemap_visual_stress PRIVATE render_features runtime_render_scene_test_support lux::engine::runtime::runtime_execution lux::engine::runtime::runtime_pack_presentation2d lux::engine::resource::asset lux::engine::platform::window)
target_include_directories(tilemap_visual_stress PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})
lux_render_link_2d_pack_sidecars(tilemap_visual_stress)

# VISUAL demo — the PLAYABLE traditional-2D line: tilemap level + procedural
# atlas sheet/atlas/clip assets + frame animation + kinematic character
# controller (WASD/Space, one-way platform). One demo covers A2-01/A2-02/P2-01.
add_executable(platformer2d_visual_demo platformer2d_visual_demo.cpp)
target_link_libraries(platformer2d_visual_demo PRIVATE render_features runtime_render_scene_test_support lux::engine::runtime::runtime_execution lux::engine::runtime::runtime_pack_presentation2d lux::engine::runtime::runtime_pack_physics2d lux::engine::ecs::physics2d lux::engine::resource::asset lux::engine::platform::window lux::engine::function::input)
target_include_directories(platformer2d_visual_demo PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})
lux_render_link_2d_pack_sidecars(platformer2d_visual_demo physics2d_meta)

# VISUAL demo — the Box2D-backed Physics2DSystem on screen: solid-tint rigid
# boxes fall, tip and STACK on a static floor (box-vs-box + rotation the swept
# solver cannot do). Assembled from presentation2d + physics2d contributions.
add_executable(rigidbody2d_visual_demo rigidbody2d_visual_demo.cpp)
target_link_libraries(rigidbody2d_visual_demo PRIVATE render_features runtime_render_scene_test_support lux::engine::runtime::runtime_execution lux::engine::runtime::runtime_pack_presentation2d lux::engine::runtime::runtime_pack_physics2d lux::engine::ecs::physics2d lux::engine::ecs::scripting lux::engine::ecs::scripting_lua lux::engine::resource::asset lux::engine::platform::window lux::engine::function::input) # scripting: Lua collision probe
target_include_directories(rigidbody2d_visual_demo PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})
lux_render_link_2d_pack_sidecars(
    rigidbody2d_visual_demo
    physics2d_meta
    script_meta
)

# VISUAL stress — the GPU-driven Canvas2D v2 ON SCREEN (2D counterpart of
# deferred_stress_test): 50k GPU-resident images + orbiting camera (one op/frame)
# + a 1k dynamic swirl (one bulk/frame) + priority-wave order rebuilds.
add_executable(canvas2d_visual_stress canvas2d_visual_stress.cpp)
target_link_libraries(canvas2d_visual_stress PRIVATE render_features lux::engine::platform::window)
target_include_directories(canvas2d_visual_stress PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

# S2-03 visual — Canvas2D images ON SCREEN (swapchain-bound, loops until closed).
# Interactive: for eyeballing the 2D pipeline, not CI. Uses DeviceRenderFixture.hpp.
add_executable(image2d_visual_demo image2d_visual_demo.cpp)
target_link_libraries(
	image2d_visual_demo
	PRIVATE
	render_features
	lux::engine::platform::window
)
target_include_directories(
	image2d_visual_demo
	PRIVATE
	${RENDER_PINCLUDE_DIR}
	${SHADER_CONFIG_OUT_PATH}
)

# ─────────────────────────────────────────────────────────────────────────────
#  CTest registration
#    gpu          — headless Vulkan; self-checking; terminate on their own.
#    interactive  — open a window and loop until closed. NOT for unattended CI;
#                   the TIMEOUT turns an accidental headless run into a fail-fast.
#  Typical CI:  ctest -LE interactive
# ─────────────────────────────────────────────────────────────────────────────

# NAME lets one executable be registered more than once under different
# arguments (see the Mobile-tier run below); it defaults to the target name.
function(lux_render_add_test target)
    cmake_parse_arguments(ARG "" "LABEL;TIMEOUT;NAME" "ARGS" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 120)
    endif()
    if(NOT ARG_NAME)
        set(ARG_NAME ${target})
    endif()
endfunction()

# Headless Vulkan stress — needs a GPU runner
lux_render_add_test(spatial_cull_grid_stress_test   LABEL gpu TIMEOUT 300)
lux_render_add_test(canvas2d_stress_test            LABEL gpu TIMEOUT 300)
lux_render_add_test(scene_cycle_stress_test         LABEL gpu TIMEOUT 600)
lux_render_add_test(upload_without_frame_test       LABEL gpu TIMEOUT 120)
lux_render_add_test(render_cluster_upload_gpu_probe LABEL gpu TIMEOUT 120)
lux_render_add_test(terrain_gpu_probe               LABEL gpu TIMEOUT 120)
lux_render_add_test(gpu_timing_probe                LABEL gpu TIMEOUT 120)
# Same executable, Mobile tier. The descriptor-set budget gate inside it only
# means something at the tier the phone actually runs, and the server resolves
# min(achievable, requested) — so asking for Mobile on a desktop box genuinely
# gets Mobile. Few cycles: this run is about the layout plan, while the
# lifecycle-churn coverage is the Desktop run's job.
lux_render_add_test(scene_cycle_stress_test         NAME scene_cycle_stress_test_mobile
                                                    LABEL gpu TIMEOUT 300
                                                    ARGS --mobile --cycles=3)

# CPU-only (no GPU runner needed)
lux_render_add_test(spirv_patcher_test              LABEL cpu TIMEOUT 60)
lux_render_add_test(shadow_atlas_placement_test     LABEL cpu TIMEOUT 60)
lux_render_add_test(gpu_layout_test                 LABEL cpu TIMEOUT 60)
lux_render_add_test(texture_sampling_representation_catalog_test LABEL cpu TIMEOUT 60)
lux_render_add_test(render_cluster_registry_test     LABEL cpu TIMEOUT 60)
lux_render_add_test(terrain_page_cache_test          LABEL cpu TIMEOUT 60)
lux_render_add_test(error_registry_test             LABEL cpu TIMEOUT 60)
lux_render_add_test(frame_driver_factory_test       LABEL cpu TIMEOUT 60)
lux_render_add_test(frame_retirement_plan_test      LABEL cpu TIMEOUT 60)
lux_render_add_test(stable_record_pages_test        LABEL cpu TIMEOUT 60)
lux_render_add_test(instance_sparse_contract_test   LABEL cpu TIMEOUT 60)
lux_render_add_test(classic_mesh_segment_contract_test LABEL cpu TIMEOUT 60)
lux_render_add_test(swapchain_factory_test           LABEL cpu TIMEOUT 60)
lux_render_add_test(condition_chain_scope_test      LABEL cpu TIMEOUT 60)
lux_render_add_test(material_instance_bucket_test   LABEL cpu TIMEOUT 60)
lux_render_add_test(feature_level_test              LABEL cpu TIMEOUT 60)
lux_render_add_test(feature_type_registry_lifetime_test LABEL cpu TIMEOUT 60)
lux_render_add_test(upload_channel_contract_test    LABEL cpu TIMEOUT 60)

# 源码扫描门禁:渲染库里一处 std::cerr / std::cout 都不能有。不需要编译目标,
# 直接由 cmake -P 跑脚本(见 no_terminal_io_test.cmake 里的理由)。
# ENGINE_STRICT_ROOTS = 已完成 lux::log 迁移的 engine 组件(§7.1),对它们
# 连裸 fprintf(stderr 也拦——迁干净的模块不许再长回来。

# Interactive executables open a window and run until a person closes it.
# They are build targets, not CTest cases: registering them made plain `ctest`
# deterministically spend ten timeout windows and report false failures. Run
# these targets explicitly for attended visual validation.
