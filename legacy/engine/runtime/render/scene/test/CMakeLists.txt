# engine/runtime/render/scene unit tests(驻留 T4R 起;-DENABLE_SCENE_RUNTIME_TEST=ON)。
#
# render_resource_manager_test — headless 自校验 exe:三件套(纯状态表 +
# 全局服务 + 引擎编排,J6-六修)契约网 —— 假域子服务 + 手动喂入口,钉
# 去重/终态/封印解封/1→0 复查/在途亡自毁/级联/RAII 等待退订/力扫。

function(lux_scene_runtime_test_enable_stdexec target)
    if(MSVC)
        target_compile_options(
            ${target}
            PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor)
    endif()
endfunction()

# The loader fixtures are declared before runtime_render_scene.  Keep this
# vertical transaction test with the first point where every participant is a
# real target; configuring it in loader/test made the old TARGET guard always
# false because CMake had not visited render/scene yet.
add_executable(engine_extensions_test engine_extensions_test.cpp)
target_link_libraries(
    engine_extensions_test
    PRIVATE
        runtime_render_scene
        runtime_render_scene_test_support
        lux::engine::runtime::runtime_execution
        lux::engine::runtime::runtime_extension_orchestration
)
lux_scene_runtime_test_enable_stdexec(engine_extensions_test)
add_dependencies(
    engine_extensions_test
    minimal_extension_module
    dependent_extension_module
    component_extension_module
    rollback_extension_module
)
add_test(
    NAME phase9_extension_metadata_transaction
    COMMAND engine_extensions_test
        $<TARGET_FILE:minimal_extension_module>
        $<TARGET_FILE:dependent_extension_module>
        $<TARGET_FILE:component_extension_module>
        $<TARGET_FILE:rollback_extension_module>
)
set_tests_properties(
    phase9_extension_metadata_transaction
    PROPERTIES LABELS "phase9;cpu"
)

add_executable(render_request_sender_test render_request_sender_test.cpp)
target_link_libraries(
    render_request_sender_test
    PRIVATE
        runtime_render_scene_test_support
        STDEXEC::stdexec
        lux::engine::function::render_features
)
lux_scene_runtime_test_enable_stdexec(render_request_sender_test)

add_executable(
    rendered_scene_transaction_test
    rendered_scene_transaction_test.cpp
)
target_link_libraries(
    rendered_scene_transaction_test
    PRIVATE
        runtime_render_scene
        runtime_render_scene_test_support
        lux::engine::runtime::runtime_execution
        lux::engine::resource::asset
)
lux_scene_runtime_test_enable_stdexec(rendered_scene_transaction_test)
add_test(
    NAME phase9_rendered_scene_transaction
    COMMAND rendered_scene_transaction_test
)
set_tests_properties(
    phase9_rendered_scene_transaction
    PROPERTIES LABELS "phase9;cpu"
)

add_executable(render_resource_manager_test render_resource_manager_test.cpp)

target_link_libraries(render_resource_manager_test
    PRIVATE
    runtime_render_scene_test_support
    lux::engine::runtime::runtime_execution          # 直接驱动 AsyncRuntime(drainMainThreadCompletions/构造)
    lux::engine::resource::asset
)
lux_scene_runtime_test_enable_stdexec(render_resource_manager_test)


add_executable(owner_reply_reaper_test owner_reply_reaper_test.cpp)
target_link_libraries(owner_reply_reaper_test PRIVATE runtime_render_scene_test_support)
lux_scene_runtime_test_enable_stdexec(owner_reply_reaper_test)

add_executable(headless_scene_runtime_test headless_scene_runtime_test.cpp)
target_link_libraries(
    headless_scene_runtime_test
    PRIVATE
        lux::engine::runtime::runtime_scene_core
        lux::engine::runtime::runtime_execution
        lux::engine::function::input
        lux::engine::resource::asset
)
lux_scene_runtime_test_enable_stdexec(headless_scene_runtime_test)
add_test(NAME phase9_headless_scene_runtime COMMAND headless_scene_runtime_test)
set_tests_properties(
    phase9_headless_scene_runtime
    PROPERTIES LABELS "phase9;cpu"
)

add_executable(
    scene_content_render_contract_test
    scene_content_render_contract_test.cpp
)
target_link_libraries(
    scene_content_render_contract_test
    PRIVATE
        lux::engine::ecs::render_content_3d_integration
        lux::engine::function::render_features
        lux::engine::function::render_standard_content
)
add_test(
    NAME phase9_scene_content_render_contract
    COMMAND scene_content_render_contract_test
)
set_tests_properties(
    phase9_scene_content_render_contract
    PROPERTIES LABELS "phase9;cpu"
)
target_include_directories(
    scene_content_render_contract_test
    PRIVATE ${CMAKE_SOURCE_DIR}/ecs/render/pinclude
)

add_executable(
    scene_geometry_prepare_service_test
    scene_geometry_prepare_service_test.cpp
)
target_link_libraries(
    scene_geometry_prepare_service_test
    PRIVATE
        runtime_render_scene_test_support
        lux::engine::runtime::runtime_execution
)
lux_scene_runtime_test_enable_stdexec(scene_geometry_prepare_service_test)
add_test(
    NAME phase9_scene_geometry_prepare_port
    COMMAND scene_geometry_prepare_service_test
)
set_tests_properties(
    phase9_scene_geometry_prepare_port
    PROPERTIES LABELS "phase9;cpu"
)

add_executable(
    render_backend_upload_close_test
    render_backend_upload_close_test.cpp
)
target_link_libraries(
    render_backend_upload_close_test
    PRIVATE
        runtime_render_backend_host
        runtime_render_scene
        runtime_render_scene_test_support
        render_features
        lux::engine::runtime::runtime_execution
)
lux_scene_runtime_test_enable_stdexec(render_backend_upload_close_test)

# residency_gpu_probe — 域子服务端到端探针:真设备往返(表+服务+编排
# 管道+真域子服务;T7 贴图起逐域扩容,现覆盖 TEXTURE/MESH)。无 Vulkan
# 自跳过(exit 0)。DeviceRenderFixture.hpp 经跨树 include 复用
# (toolchain_shader_compiler/test 先例:modules 层测试夹具,engine 层测试目标
# 在这里接线)。
add_executable(residency_gpu_probe residency_gpu_probe.cpp)

target_link_libraries(residency_gpu_probe
    PRIVATE
    runtime_render_scene_test_support
    lux::engine::runtime::runtime_execution                          # AsyncRuntime
    lux::engine::platform::window      # DeviceRenderFixture 的窗口面
    lux::engine::resource::asset
)
lux_scene_runtime_test_enable_stdexec(residency_gpu_probe)

target_include_directories(residency_gpu_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/modules/function/render/test)

add_executable(
    scene_visual_components_semantic_gpu_probe
    scene_visual_components_semantic_gpu_probe.cpp
)
set(
    SCENE_VISUAL_COMPONENTS_GBUFFER_SPV
    ${CMAKE_CURRENT_BINARY_DIR}/scene_visual_components_gbuffer.frag.spv
)
set(
    SCENE_VISUAL_COMPONENTS_FORWARD_SPV
    ${CMAKE_CURRENT_BINARY_DIR}/scene_visual_components_forward.frag.spv
)
add_custom_command(
    OUTPUT ${SCENE_VISUAL_COMPONENTS_GBUFFER_SPV}
    COMMAND ${Vulkan_GLSLC_EXECUTABLE}
        --target-env=vulkan1.2
        -fshader-stage=frag
        ${CMAKE_SOURCE_DIR}/modules/function/render/test/render_3d_semantic_gbuffer.frag
        -o ${SCENE_VISUAL_COMPONENTS_GBUFFER_SPV}
    DEPENDS
        ${CMAKE_SOURCE_DIR}/modules/function/render/test/render_3d_semantic_gbuffer.frag
    COMMENT "Compiling scene visual component semantic GBuffer shader"
    VERBATIM
)
add_custom_command(
    OUTPUT ${SCENE_VISUAL_COMPONENTS_FORWARD_SPV}
    COMMAND ${Vulkan_GLSLC_EXECUTABLE}
        --target-env=vulkan1.2
        -fshader-stage=frag
        ${CMAKE_SOURCE_DIR}/modules/function/render/test/render_3d_semantic.frag
        -o ${SCENE_VISUAL_COMPONENTS_FORWARD_SPV}
    DEPENDS
        ${CMAKE_SOURCE_DIR}/modules/function/render/test/render_3d_semantic.frag
    COMMENT "Compiling scene visual component semantic forward shader"
    VERBATIM
)
add_custom_target(
    scene_visual_components_semantic_shader
    DEPENDS
        ${SCENE_VISUAL_COMPONENTS_GBUFFER_SPV}
        ${SCENE_VISUAL_COMPONENTS_FORWARD_SPV}
)
add_dependencies(
    scene_visual_components_semantic_gpu_probe
    scene_visual_components_semantic_shader
)
target_link_libraries(
    scene_visual_components_semantic_gpu_probe
    PRIVATE
        runtime_render_scene_test_support
        lux::engine::function::render_features
        lux::engine::platform::window
)
target_include_directories(
    scene_visual_components_semantic_gpu_probe
    PRIVATE ${CMAKE_SOURCE_DIR}/modules/function/render/test
)
target_compile_definitions(
    scene_visual_components_semantic_gpu_probe
    PRIVATE
        LUX_SCENE_VISUAL_COMPONENTS_GBUFFER_SPV="${SCENE_VISUAL_COMPONENTS_GBUFFER_SPV}"
        LUX_SCENE_VISUAL_COMPONENTS_FORWARD_SPV="${SCENE_VISUAL_COMPONENTS_FORWARD_SPV}"
)

# The GPU probe compiles the exact same scenario body as the headless gate.
# Its extension only installs the presentation leaf and observes checkpoints;
# EntityScene generation, Section residency selection, ECS publication,
# persistence and close therefore cannot drift into a hand-built visual path.
set(
    INFINITE2D_TEST_DIR
    ${CMAKE_SOURCE_DIR}/engine/runtime/assets/pixel/test
)
add_executable(
    infinite2d_visual_gpu_probe
    ${INFINITE2D_TEST_DIR}/infinite2d_headless_test.cpp
    ${INFINITE2D_TEST_DIR}/infinite2d_visual_gpu_probe.cpp
)
target_include_directories(
    infinite2d_visual_gpu_probe
    PRIVATE
        ${INFINITE2D_TEST_DIR}
        ${CMAKE_SOURCE_DIR}/modules/function/render/test
)
target_link_libraries(
    infinite2d_visual_gpu_probe
    PRIVATE
        runtime_render_scene_test_support
        lux::engine::runtime::runtime_pixel_assets
        lux::engine::ecs::pixel_streaming
        lux::engine::runtime::runtime_execution
        lux::engine::runtime::runtime_tilemap_prepare
        lux::engine::ecs::tilemap_streaming
        lux::engine::ecs::tilemap
        lux::engine::ecs::render_2d
        lux::engine::ecs::render_pixel_integration
        lux::engine::function::render_features
        lux::engine::resource::asset
        lux::engine::resource::asset
        lux::engine::core::serialization
        lux::engine::ecs::component_archive
        lux::engine::platform::window
)
if(MSVC)
    target_compile_options(infinite2d_visual_gpu_probe PRIVATE /UNDEBUG)
else()
    target_compile_options(infinite2d_visual_gpu_probe PRIVATE -UNDEBUG)
endif()
lux_scene_runtime_test_enable_stdexec(infinite2d_visual_gpu_probe)
