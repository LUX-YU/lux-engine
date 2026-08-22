include_component_cmake_scripts(meta)
include_component_cmake_scripts(lux::engine::resource::asset)

set(RENDER_PINCLUDE_DIR "${CMAKE_SOURCE_DIR}/modules/function/render/vulkan/pinclude")
set(UI_TEST_CONFIG_OUT  "${CMAKE_BINARY_DIR}/gen/test/function/ui/include")

component_generate_runtime_asset_path_header(
    COMPONENT render_features
    OUTPUT_HEADER ${UI_TEST_CONFIG_OUT}/test_time_asset_path.hpp
)

find_package(imgui REQUIRED COMPONENTS glfw3 vulkan)

# ── ImGui comm pipeline end-to-end render test ────────────────────────────────
add_executable(
	imgui_comm_render_test
	${CMAKE_CURRENT_SOURCE_DIR}/imgui_comm_render_test.cpp
)

target_link_libraries(
	imgui_comm_render_test
	PRIVATE
		lux::engine::function::ui_vulkan
)

# scene_view_integration_test -> RELOCATED to engine/toolchain/shader/glsl/test (W5a:
# ported off rdesc::Material onto graph materials; needs the engine-tier cook backend,
# which a modules/-tier ui test target can't link. Sources stay here.)

# ── Swapchain scene direct rendering test ─────────────────────────────────
add_executable(
	swapchain_scene_test
	${CMAKE_CURRENT_SOURCE_DIR}/swapchain_scene_test.cpp
)

target_include_directories(
	swapchain_scene_test
	PRIVATE
		${UI_TEST_CONFIG_OUT}
		${RENDER_PINCLUDE_DIR}
)

target_link_libraries(
	swapchain_scene_test
	PRIVATE
		lux::engine::function::ui_vulkan
)
