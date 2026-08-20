# toolchain_shader_compiler engine-half tests (self-checking, exit 0 = pass).
# Enable with: -DENABLE_SHADERGEN_GLSL_TEST=ON (requires -DLUX_ENABLE_SHADERGEN=ON).
# Device-independent: only emits + compiles to SPIR-V via shaderc, no VkDevice created.

add_executable(toolchain_shader_compiler_emit_test emit_test.cpp)
target_link_libraries(toolchain_shader_compiler_emit_test
    PRIVATE
    toolchain_shader_compiler
    toolchain_material_compiler
    lux::engine::resource::description
)

# The IncluderInterface's search path = render's shared GLSL single-source-of-truth
# directory (gbuffer_encode.glsl, etc.). Injected by CMake so the test doesn't
# hard-code an absolute path; toolchain_shader_compiler itself has no dependency on render.
target_compile_definitions(toolchain_shader_compiler_emit_test
    PRIVATE
    LUX_RENDER_SHADERS_DIR="${CMAKE_SOURCE_DIR}/modules/function/render/features/assets/shaders"
    LUX_LGLSL_EMITTED_DIR="${CMAKE_BINARY_DIR}/lglsl_emitted"
)

# spirv-cross reflection reconciliation test -- reflects the real descriptor
# layout of the SPIR-V that shadergen produces, and checks it against the
# hand-filled ShaderInfo ("ShaderInfo doesn't lie"). spirv-cross-core is a
# find_package target from resource::asset (the resource layer is configured
# before engine); the find here is idempotent.
find_package(spirv_cross_core CONFIG REQUIRED)
add_executable(toolchain_shader_compiler_reflect_test reflect_test.cpp)
target_link_libraries(toolchain_shader_compiler_reflect_test
    PRIVATE
    toolchain_shader_compiler
    toolchain_material_compiler
    lux::engine::resource::description
    spirv-cross-core
)
target_compile_definitions(toolchain_shader_compiler_reflect_test
    PRIVATE
    LUX_RENDER_SHADERS_DIR="${CMAKE_SOURCE_DIR}/modules/function/render/features/assets/shaders"
    LUX_LGLSL_EMITTED_DIR="${CMAKE_BINARY_DIR}/lglsl_emitted"
)

# ── Tests migrated in from the retired material_graph_glsl/test ──────────────
# Compiles graph materials in-process via lux::mgtest::compileGraph (ShaderGen)
# from graph_test_helpers.hpp. Since ShaderGen #includes the real
# single-source-of-truth files, every target here injects render's GLSL
# single-source-of-truth directory.
set(_sg_shaders_dir "${CMAKE_SOURCE_DIR}/modules/function/render/features/assets/shaders")
# lighting_common 等共享头已迁 .lglslh(描述符线 0c54f94),#include 的 .glsl
# 由 lglsl 发射流程生成在 build 树;与编辑器烘焙同序:发射目录优先、源树兜底。
set(_sg_emitted_dir "${CMAKE_BINARY_DIR}/lglsl_emitted")

# Device-independent: SPIR-V + ShaderInfo .luxasset persistence round-trip (ShaderSerDeser).
add_executable(shader_asset_test graph_shader_asset_test.cpp)
target_link_libraries(shader_asset_test PRIVATE
    toolchain_shader_compiler toolchain_material_compiler lux::engine::resource::asset)
target_include_directories(shader_asset_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(shader_asset_test PRIVATE LUX_RENDER_SHADERS_DIR="${_sg_shaders_dir}"
        LUX_LGLSL_EMITTED_DIR="${_sg_emitted_dir}")

# Device-independent: ImportedMaterialDesc -> graph -> compile (Forward + GBuffer), a byte-level dedup proof.
add_executable(material_to_graph_test material_to_graph_test.cpp)
target_link_libraries(material_to_graph_test PRIVATE
    toolchain_shader_compiler toolchain_material_compiler)
target_include_directories(material_to_graph_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(material_to_graph_test PRIVATE LUX_RENDER_SHADERS_DIR="${_sg_shaders_dir}"
        LUX_LGLSL_EMITTED_DIR="${_sg_emitted_dir}")

# Headless raw-Vulkan pipeline (the generated gbuffer frag -> a valid 3-MRT VkPipeline).
find_package(Vulkan REQUIRED)
add_executable(gbuffer_pipeline_test gbuffer_pipeline_test.cpp)
target_link_libraries(gbuffer_pipeline_test PRIVATE
    toolchain_shader_compiler toolchain_material_compiler Vulkan::Vulkan)
target_include_directories(gbuffer_pipeline_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(gbuffer_pipeline_test PRIVATE LUX_RENDER_SHADERS_DIR="${_sg_shaders_dir}"
        LUX_LGLSL_EMITTED_DIR="${_sg_emitted_dir}")

# Rendering tests (require render_features + window; skipped when no device is available).
if(TARGET render_features AND TARGET lux::engine::platform::window)
    foreach(_t gbuffer_deferred_lit_test textured_material_test graph_params_render_test
               graph_forward_render_test graph_distinct_pso_render_test)
        add_executable(${_t} ${_t}.cpp)
        target_link_libraries(${_t} PRIVATE
            toolchain_shader_compiler toolchain_material_compiler
            render_features lux::engine::platform::window)
        target_include_directories(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(${_t} PRIVATE LUX_RENDER_SHADERS_DIR="${_sg_shaders_dir}"
        LUX_LGLSL_EMITTED_DIR="${_sg_emitted_dir}")
    endforeach()

    # Relocated render/ui tests (the source lives in the render/ui test
    # directory; only the add_executable is here -- since modules/ is
    # configured before engine/, a modules-tier test target can't link this
    # engine-tier lib, so the wiring lives here instead). The include path
    # adds this test directory to pick up graph_test_helpers.hpp.
    set(_rt_dir      ${CMAKE_SOURCE_DIR}/modules/function/render/test)
    set(_rt_gen      ${CMAKE_BINARY_DIR}/gen/test/function/render/include)
    set(_rt_pinclude ${CMAKE_SOURCE_DIR}/modules/function/render/vulkan/pinclude)
    foreach(_t game_loop_test two_scene_shadow_isolation_test
               deferred_multi_shading_model_test deferred_stress_test)
        add_executable(${_t} ${_rt_dir}/${_t}.cpp)
        target_link_libraries(${_t} PRIVATE
            toolchain_shader_compiler toolchain_material_compiler
            render_features lux::engine::platform::window
            lux::engine::resource::asset)
        target_include_directories(${_t} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR} ${_rt_gen} ${_rt_pinclude})
        target_compile_definitions(${_t} PRIVATE LUX_RENDER_SHADERS_DIR="${_sg_shaders_dir}"
        LUX_LGLSL_EMITTED_DIR="${_sg_emitted_dir}")
    endforeach()

    # skeletal_mesh_walk also needs lux::animation (buildSkinningMatrices).
    add_executable(skeletal_mesh_walk_test ${_rt_dir}/skeletal_mesh_walk_test.cpp)
    target_link_libraries(skeletal_mesh_walk_test PRIVATE
        toolchain_shader_compiler toolchain_material_compiler
        render_features animation_3d lux::engine::platform::window
        lux::engine::resource::asset)
    target_include_directories(skeletal_mesh_walk_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${_rt_gen} ${_rt_pinclude})
    target_compile_definitions(skeletal_mesh_walk_test PRIVATE LUX_RENDER_SHADERS_DIR="${_sg_shaders_dir}"
        LUX_LGLSL_EMITTED_DIR="${_sg_emitted_dir}")

    # scene_view_integration is a UI integration test -> also link ui + its ui-test gen dir.
    if(TARGET lux::engine::function::ui_vulkan)
        add_executable(scene_view_integration_test
            ${CMAKE_SOURCE_DIR}/modules/function/ui/test/scene_view_integration_test.cpp)
        target_link_libraries(scene_view_integration_test PRIVATE
            toolchain_shader_compiler toolchain_material_compiler
            lux::engine::function::ui_vulkan)
        target_include_directories(scene_view_integration_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_BINARY_DIR}/gen/test/function/ui/include
            ${_rt_pinclude})
        target_compile_definitions(scene_view_integration_test PRIVATE LUX_RENDER_SHADERS_DIR="${_sg_shaders_dir}"
        LUX_LGLSL_EMITTED_DIR="${_sg_emitted_dir}")
    endif()
else()
    message(STATUS "toolchain_shader_compiler: skipping migrated render_features tests (render/window unavailable)")
endif()
