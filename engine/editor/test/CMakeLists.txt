## engine/editor/test — opt-in smoke tests for the editor library.
##
## Gated by ENABLE_EDITOR_TEST (set in engine/editor/CMakeLists.txt) so a
## default build doesn't compile or link them. Each test is a standalone
## executable runnable from the build tree.

add_executable(
    editor_tool_catalog_test
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_tool_catalog_test.cpp
)

target_link_libraries(editor_tool_catalog_test PRIVATE editor)

add_executable(
    editor_camera_large_coordinate_test
    ${CMAKE_CURRENT_SOURCE_DIR}/editor_camera_large_coordinate_test.cpp
)
lux_classify_target(
    TARGET  editor_camera_large_coordinate_test
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    DOMAIN
)
target_link_libraries(editor_camera_large_coordinate_test PRIVATE editor)

# A real editor-only MODULE fixture. This locks the public SDK seam across a
# DLL boundary: ABI descriptor -> editor registrar draft -> atomic catalog
# publication -> panel factory, while proving it cannot satisfy Runtime.
add_library(editor_extension_module MODULE editor_extension_module.cpp)
lux_classify_target(
    TARGET  editor_extension_module
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    DOMAIN
)
target_link_libraries(editor_extension_module PRIVATE editor)
set_target_properties(
    editor_extension_module
    PROPERTIES
        PREFIX ""
        OUTPUT_NAME "org.lux.test.editor-module"
)

add_executable(
    editor_extension_module_test
    editor_extension_module_test.cpp
)
lux_classify_target(
    TARGET  editor_extension_module_test
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    COMPOSITION
)
target_link_libraries(editor_extension_module_test PRIVATE editor)
add_dependencies(editor_extension_module_test editor_extension_module)

# ── Phase B smoke: Project + ProjectManifest round-trip ─────────────────
add_executable(
    project_roundtrip_smoke
    ${CMAKE_CURRENT_SOURCE_DIR}/project_roundtrip_smoke.cpp
)
target_link_libraries(
    project_roundtrip_smoke
    PRIVATE
        editor
)

# ── Phase D smoke: Scene save / load round-trip ─────────────────────────
add_executable(
    scene_roundtrip_smoke
    ${CMAKE_CURRENT_SOURCE_DIR}/scene_roundtrip_smoke.cpp
)
target_link_libraries(
    scene_roundtrip_smoke
    PRIVATE
        editor
        lux::engine::toolchain::toolchain_spatial3d_scene_cook
)

# Force-include the gameplay meta sidecar so the reflection + component-type
# registries are populated when the test runs (all component reflection — neutral
# core + 3D — lives in gameplay_meta again).
if(TARGET ecs_meta)
    target_link_libraries(scene_roundtrip_smoke PRIVATE ecs_meta)
endif()
foreach(_sc
        render_components_meta
        render_components_2d_meta
        render_components_3d_meta
        spatial3d_components_meta
        pixel_meta
        animation_2d_meta
        physics2d_meta)
    if(TARGET ${_sc})
        target_link_libraries(scene_roundtrip_smoke PRIVATE ${_sc})
    endif()
endforeach()

add_executable(
    world_persistence_schema_closure_test
    ${CMAKE_CURRENT_SOURCE_DIR}/world_persistence_schema_closure_test.cpp
)
lux_classify_target(
    TARGET  world_persistence_schema_closure_test
    LAYER   EDITOR
    PRODUCT TEST
    ROLE    DOMAIN
)
target_link_libraries(
    world_persistence_schema_closure_test
    PRIVATE editor
)
target_include_directories(
    world_persistence_schema_closure_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# ── Thumbnail storage smoke: PNG codec + ThumbnailSet + payload round-trip ──
add_executable(
    thumbnail_payload_smoke
    ${CMAKE_CURRENT_SOURCE_DIR}/thumbnail_payload_smoke.cpp
)
target_link_libraries(
    thumbnail_payload_smoke
    PRIVATE
        editor
)

# ── Thumbnail GPU test: ThumbnailService (私有预览 SceneRuntime) 全类型端到端 ──
add_executable(
    thumbnail_render_gpu_test
    ${CMAKE_CURRENT_SOURCE_DIR}/thumbnail_render_gpu_test.cpp
)
target_link_libraries(
    thumbnail_render_gpu_test
    PRIVATE
        editor
        runtime_render_scene_test_support
        lux::engine::function::render_features
        lux::engine::platform::window
        lux::engine::resource::asset
)
target_include_directories(
    thumbnail_render_gpu_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# ── Emissive palette SPIR-V identity: guards the "bake once, reuse" precondition ──
#
# 这条是 EditorBuiltins::registerEmissivePalette 去重的前提守卫,不是可选 smoke:
# 去重成立当且仅当颜色不进着色器。所以它 add_test 进 ctest 的 cpu 档,跟着门禁跑。
# 无设备无窗口,纯 CPU(lower + shaderc)。
add_executable(
    emissive_palette_spirv_identity_test
    ${CMAKE_CURRENT_SOURCE_DIR}/emissive_palette_spirv_identity_test.cpp
)
target_link_libraries(
    emissive_palette_spirv_identity_test
    PRIVATE
        editor
)
# compileGraphToPayload 住在 editor 的私有 src/ 里(editor 只 PUBLIC 暴露 include/),
# 测试要直接调它就得自己把 src/ 加进搜索路径。
target_include_directories(
    emissive_palette_spirv_identity_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

# ── 驻留 T9/T10 gpu 探针:材质双域端到端(真设备;无 Vulkan 自跳过)──
#
# 放这里的原因:烘焙入口 compileGraphToPayload 是 editor 私有 src 的能力
# (同上一条的 include 说明);DeviceRenderFixture 经跨树 include 复用
# (toolchain_shader_compiler/test 先例)。
add_executable(
    material_residency_gpu_probe
    ${CMAKE_CURRENT_SOURCE_DIR}/material_residency_gpu_probe.cpp
)
target_link_libraries(
    material_residency_gpu_probe
    PRIVATE
        editor
        runtime_render_scene_test_support
        lux::engine::runtime::runtime_execution
        lux::engine::function::render_features
        lux::engine::platform::window
        lux::engine::resource::asset
)
target_include_directories(
    material_residency_gpu_probe
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_SOURCE_DIR}/modules/function/render/test
)
