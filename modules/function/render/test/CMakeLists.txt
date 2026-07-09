set(SHADER_COMPILE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/gen/shaders)
set(SHADER_CONFIG_OUT_PATH     ${CMAKE_BINARY_DIR}/gen/test/function/render/include)
set(RENDER_PINCLUDE_DIR        ${CMAKE_CURRENT_SOURCE_DIR}/../pinclude)

find_package(imgui REQUIRED COMPONENTS glfw3 vulkan)

component_generate_runtime_asset_path_header(
    COMPONENT render
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
#  engine/shadergen_glsl/test (W5a: they need the graph-material cook backend).
#
#  skeletal_world_demo -> DISABLED (W5c): still authors the deleted closure
#  rdesc::Material path; source kept in-tree pending a relocation+port to graph
#  materials.
# ─────────────────────────────────────────────────────────────────────────────

add_executable(
	point_cloud_stress_test
	point_cloud_stress_test.cpp
)

target_link_libraries(
	point_cloud_stress_test
	PRIVATE
	render
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
	render
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
	render
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
	render
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
    render
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
target_link_libraries(canvas2d_stress_test PRIVATE render lux::engine::platform::window)
target_include_directories(canvas2d_stress_test PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

# VISUAL FULL-STACK stress — a living pixel world ON SCREEN: real gameplay World
# (fixed-step CA: raining sand, dripping water, digs) + ~700 sprites interleaved
# with the field on ONE priority axis (cross-kind pops) + breathing camera, all
# through the real bridges and wire. The Slice C showcase.
add_executable(pixel_world_visual_stress pixel_world_visual_stress.cpp)
target_link_libraries(pixel_world_visual_stress PRIVATE render gameplay lux::engine::platform::window)
target_include_directories(pixel_world_visual_stress PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

# VISUAL stress — the GPU-driven Canvas2D v2 ON SCREEN (2D counterpart of
# deferred_stress_test): 50k GPU-resident sprites + orbiting camera (one op/frame)
# + a 1k dynamic swirl (one bulk/frame) + priority-wave order rebuilds.
add_executable(canvas2d_visual_stress canvas2d_visual_stress.cpp)
target_link_libraries(canvas2d_visual_stress PRIVATE render lux::engine::platform::window)
target_include_directories(canvas2d_visual_stress PRIVATE ${RENDER_PINCLUDE_DIR} ${SHADER_CONFIG_OUT_PATH})

# S2-03 visual — Canvas2D sprites ON SCREEN (swapchain-bound, loops until closed).
# Interactive: for eyeballing the 2D pipeline, not CI. Uses DeviceRenderFixture.hpp.
add_executable(sprite2d_visual_demo sprite2d_visual_demo.cpp)
target_link_libraries(
	sprite2d_visual_demo
	PRIVATE
	render
	lux::engine::platform::window
)
target_include_directories(
	sprite2d_visual_demo
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

function(lux_render_add_test target)
    cmake_parse_arguments(ARG "" "LABEL;TIMEOUT" "" ${ARGN})
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 120)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES
        LABELS   "${ARG_LABEL}"
        TIMEOUT  ${ARG_TIMEOUT})
endfunction()

# Headless Vulkan stress — needs a GPU runner
lux_render_add_test(spatial_cull_grid_stress_test   LABEL gpu TIMEOUT 300)
lux_render_add_test(canvas2d_stress_test            LABEL gpu TIMEOUT 300)

# Interactive (open a window, loop until closed) — NOT for unattended CI
lux_render_add_test(point_cloud_stress_test         LABEL interactive)
lux_render_add_test(point_cloud_transient_test      LABEL interactive)
lux_render_add_test(trajectory_line_test            LABEL interactive)
lux_render_add_test(gizmo_transient_test            LABEL interactive)
lux_render_add_test(sprite2d_visual_demo            LABEL interactive)
lux_render_add_test(canvas2d_visual_stress          LABEL interactive)
lux_render_add_test(pixel_world_visual_stress       LABEL interactive)
