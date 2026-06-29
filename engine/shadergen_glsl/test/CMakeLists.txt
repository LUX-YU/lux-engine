# shadergen_glsl engine 半测试（self-checking，exit 0 = pass）。
# 启用：-DENABLE_SHADERGEN_GLSL_TEST=ON（需 -DLUX_ENABLE_SHADERGEN=ON）。
# 设备无关：只 emit + shaderc 编译到 SPIR-V，不建 VkDevice。

add_executable(shadergen_glsl_emit_test emit_test.cpp)
target_link_libraries(shadergen_glsl_emit_test
    PRIVATE
    shadergen_glsl
    shadergen
    lux::engine::resource::description
)

# IncluderInterface 的搜索路径 = render 的共享 GLSL SSOT 目录（gbuffer_encode.glsl 等）。
# 由 CMake 注入，测试不硬编码绝对路径；shadergen_glsl 本身不依赖 render。
target_compile_definitions(shadergen_glsl_emit_test
    PRIVATE
    LUX_RENDER_SHADERS_DIR="${CMAKE_SOURCE_DIR}/modules/function/render/assets/shaders"
)

# 里程碑④：spirv-cross 反射对账测试 —— 反射 shadergen 产出 SPIR-V 的真实 descriptor
# 布局，对照手填的 ShaderInfo（"ShaderInfo 不撒谎"）。spirv-cross-core 是 resource::asset
# 的 find_package target（resource 层先于 engine 配置）；此处 find 幂等。
find_package(spirv_cross_core CONFIG REQUIRED)
add_executable(shadergen_glsl_reflect_test reflect_test.cpp)
target_link_libraries(shadergen_glsl_reflect_test
    PRIVATE
    shadergen_glsl
    shadergen
    lux::engine::resource::description
    spirv-cross-core
)
target_compile_definitions(shadergen_glsl_reflect_test
    PRIVATE
    LUX_RENDER_SHADERS_DIR="${CMAKE_SOURCE_DIR}/modules/function/render/assets/shaders"
)

# ── 里程碑⑤：从退役的 material_graph_glsl/test 迁入的测试 ──────────────────────
# 经 graph_test_helpers.hpp 的 lux::mgtest::compileGraph（ShaderGen）in-process 编译图
# 材质。ShaderGen #include 真 SSOT，故每个 target 注入 render 的 GLSL SSOT 目录。
set(_sg_shaders_dir "${CMAKE_SOURCE_DIR}/modules/function/render/assets/shaders")

# 设备无关：SPIR-V + ShaderInfo .luxasset 持久化往返（ShaderSerDeser）。
add_executable(shader_asset_test graph_shader_asset_test.cpp)
target_link_libraries(shader_asset_test PRIVATE
    shadergen_glsl shadergen lux::engine::resource::asset)
target_include_directories(shader_asset_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(shader_asset_test PRIVATE LUX_RENDER_SHADERS_DIR="${_sg_shaders_dir}")

# 设备无关：ImportedMaterialDesc -> graph -> 编译（Forward + GBuffer），字节级去重证明。
add_executable(material_to_graph_test material_to_graph_test.cpp)
target_link_libraries(material_to_graph_test PRIVATE shadergen_glsl shadergen)
target_include_directories(material_to_graph_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(material_to_graph_test PRIVATE LUX_RENDER_SHADERS_DIR="${_sg_shaders_dir}")

# headless raw-Vulkan pipeline（生成的 gbuffer frag -> valid 3-MRT VkPipeline）。
find_package(Vulkan REQUIRED)
add_executable(gbuffer_pipeline_test gbuffer_pipeline_test.cpp)
target_link_libraries(gbuffer_pipeline_test PRIVATE shadergen_glsl shadergen Vulkan::Vulkan)
target_include_directories(gbuffer_pipeline_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(gbuffer_pipeline_test PRIVATE LUX_RENDER_SHADERS_DIR="${_sg_shaders_dir}")

# 渲染测试（需 render + window；无 device 时 skip）。
if(TARGET render AND TARGET lux::engine::platform::window)
    foreach(_t gbuffer_deferred_lit_test textured_material_test graph_params_render_test
               graph_forward_render_test graph_distinct_pso_render_test)
        add_executable(${_t} ${_t}.cpp)
        target_link_libraries(${_t} PRIVATE
            shadergen_glsl shadergen render lux::engine::platform::window)
        target_include_directories(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(${_t} PRIVATE LUX_RENDER_SHADERS_DIR="${_sg_shaders_dir}")
    endforeach()

    # W5a relocated render/ui tests（源在 render/ui 测试目录，只 add_executable 在这；
    # modules/ 先于 engine/ 配置，故 modules-tier test target 无法 link 此 engine-tier
    # lib —— 编排在此）。include 目录加上本 test 目录拿 graph_test_helpers.hpp。
    set(_rt_dir      ${CMAKE_SOURCE_DIR}/modules/function/render/test)
    set(_rt_gen      ${CMAKE_BINARY_DIR}/gen/test/function/render/include)
    set(_rt_pinclude ${CMAKE_SOURCE_DIR}/modules/function/render/pinclude)
    foreach(_t game_loop_test two_scene_shadow_isolation_test
               deferred_multi_shading_model_test deferred_stress_test)
        add_executable(${_t} ${_rt_dir}/${_t}.cpp)
        target_link_libraries(${_t} PRIVATE
            shadergen_glsl shadergen render lux::engine::platform::window lux::engine::resource::asset)
        target_include_directories(${_t} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR} ${_rt_gen} ${_rt_pinclude})
        target_compile_definitions(${_t} PRIVATE LUX_RENDER_SHADERS_DIR="${_sg_shaders_dir}")
    endforeach()

    # skeletal_mesh_walk also needs lux::animation (buildSkinningMatrices).
    add_executable(skeletal_mesh_walk_test ${_rt_dir}/skeletal_mesh_walk_test.cpp)
    target_link_libraries(skeletal_mesh_walk_test PRIVATE
        shadergen_glsl shadergen render animation lux::engine::platform::window lux::engine::resource::asset)
    target_include_directories(skeletal_mesh_walk_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${_rt_gen} ${_rt_pinclude})
    target_compile_definitions(skeletal_mesh_walk_test PRIVATE LUX_RENDER_SHADERS_DIR="${_sg_shaders_dir}")

    # scene_view_integration is a UI integration test -> also link ui + its ui-test gen dir.
    if(TARGET lux::engine::function::ui)
        add_executable(scene_view_integration_test
            ${CMAKE_SOURCE_DIR}/modules/function/ui/test/scene_view_integration_test.cpp)
        target_link_libraries(scene_view_integration_test PRIVATE
            shadergen_glsl shadergen lux::engine::function::ui)
        target_include_directories(scene_view_integration_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_BINARY_DIR}/gen/test/function/ui/include
            ${_rt_pinclude})
        target_compile_definitions(scene_view_integration_test PRIVATE LUX_RENDER_SHADERS_DIR="${_sg_shaders_dir}")
    endif()
else()
    message(STATUS "shadergen_glsl: skipping migrated render tests (render/window unavailable)")
endif()
