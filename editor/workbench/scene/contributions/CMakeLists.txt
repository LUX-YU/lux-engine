include_component_cmake_scripts(meta)

set(_render_source ${PROJECT_SOURCE_DIR}/engine/scene/builtin_systems/render)
generate_visibility_header(ENABLE_MACRO_NAME LUX_RENDER_EDITOR_LIBRARY
    PUBLIC_MACRO_NAME LUX_RENDER_EDITOR_PUBLIC
    GENERATE_FILE_PATH lux/engine/editor/extensions/builtin/render_visibility.h)
add_component(COMPONENT_NAME scene_render_meta NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_scene_render_meta SOURCE_FILES src/RenderSystemEditorExports.cpp)
component_include_directories(scene_render_meta BUILD_TIME_EXPORT ${LUX_GENERATE_HEADER_DIR} INSTALL_TIME include)
lux_classify_target(TARGET scene_render_meta LAYER EDITOR PRODUCT EDITOR ROLE PLUGIN)
target_compile_definitions(scene_render_meta PRIVATE LUX_RENDER_EDITOR_LIBRARY)
target_link_libraries(scene_render_meta PRIVATE scene_render lux::engine::core::meta editor_extensions)
engine_add_type_runtime_info(NAME scene_runtime_render_configuration_meta EXPLICIT_REGISTRATION
    REGISTER_FUNC_NAME scene_runtime_render_configuration REGISTER_FUNC_MACRO LUX_RENDER_EDITOR_PUBLIC
    CUSTOM_INCLUDE "lux/engine/editor/extensions/builtin/render_visibility.h"
    TARGET_FILES ${_render_source}/include/lux/engine/scene/RenderSystemConfiguration.hpp)
engine_target_add_type_runtime_info(TARGET scene_render_meta METAS scene_runtime_render_configuration_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/render_gen TOP_LEVEL_REGISTER_FUNC_PREFIX LuxRegisterSceneRuntimeRenderMetas)

set(_feature_root "${PROJECT_SOURCE_DIR}/modules/function/render/features")
include(${_feature_root}/cmake/render_comm_operations.cmake)
add_component(COMPONENT_NAME render_feature_meta NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_function_render_feature_meta
    SOURCE_FILES src/RenderFeatureEditorExports.cpp)
component_include_directories(render_feature_meta
    BUILD_TIME_EXPORT ${_feature_root}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include)
target_compile_definitions(render_feature_meta PRIVATE LUX_RENDER_FEATURE_META_LIBRARY)
target_link_libraries(render_feature_meta PRIVATE lux::engine::editor::scene_ui render_feature_client lux::engine::core::meta
    lux::engine::editor::editor_extensions lux::engine::function::ui)
lux_classify_target(TARGET render_feature_meta LAYER EDITOR PRODUCT EDITOR ROLE PLUGIN)
component_add_transitive_commands(render_feature_meta
    "find_package(lux-engine-function REQUIRED COMPONENTS render_feature_client)")
engine_enable_module_meta(EXPLICIT_REGISTRATION TARGET render_feature_client SIDECAR_TARGET render_feature_meta
    REGISTER_FUNC_MACRO LUX_RENDER_FEATURE_META_PUBLIC
    VISIBILITY_HEADER lux/engine/function/render/features/meta_visibility.h
    TARGET_FILES ${LUX_RENDER_COMM_OPERATION_HEADERS}
        ${_feature_root}/include/lux/engine/function/render/features/postprocess/TonemapParams.hpp
        ${_feature_root}/include/lux/engine/function/render/features/spatialcull/SpatialCullParams.hpp
        ${_feature_root}/include/lux/engine/function/render/features/shadow/ShadowQualityParams.hpp)

set(_components scene_render_meta render_feature_meta)
if(TARGET physics2d_simulation)
    generate_visibility_header(ENABLE_MACRO_NAME LUX_PHYSICS2D_EDITOR_LIBRARY
        PUBLIC_MACRO_NAME LUX_PHYSICS2D_EDITOR_PUBLIC
        GENERATE_FILE_PATH lux/engine/editor/extensions/builtin/physics2d_visibility.h)
    add_component(COMPONENT_NAME physics2d_editor NAMESPACE lux::engine::editor
        SOURCE_FILES src/Physics2DEditorExports.cpp)
    component_include_directories(physics2d_editor BUILD_TIME_EXPORT ${LUX_GENERATE_HEADER_DIR} INSTALL_TIME include)
    lux_classify_target(TARGET physics2d_editor LAYER EDITOR PRODUCT EDITOR ROLE PLUGIN)
    target_compile_definitions(physics2d_editor PRIVATE LUX_PHYSICS2D_EDITOR_LIBRARY)
    target_link_libraries(physics2d_editor PRIVATE physics2d_simulation editor_extensions scene_control_api lux::engine::function::ui)
    engine_add_type_runtime_info(NAME physics2d_editor_configuration EXPLICIT_REGISTRATION
        TARGET_FILES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/builtin_systems/physics2d/include/lux/engine/physics2d/Physics2DSystem.hpp
        REGISTER_FUNC_NAME lux_physics2d_configuration)
    engine_target_add_type_runtime_info(TARGET physics2d_editor METAS physics2d_editor_configuration
        OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/physics_gen)
    list(APPEND _components physics2d_editor)
    if(TARGET plugin_library_test)
        add_dependencies(plugin_library_test physics2d_editor)
    endif()
endif()
lux_engine_install_components(PROJECT_NAME lux-engine-editor-plugins VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS ${_components})

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(plugin_configuration_test test/configuration.cpp)
    target_link_libraries(plugin_configuration_test PRIVATE editor_extensions scene_ui lux::engine::function::ui imgui::core)

    if(MSVC)
        target_compile_options(plugin_configuration_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(plugin_configuration_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME plugin.configuration COMMAND plugin_configuration_test )
    set_tests_properties(plugin.configuration PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "editor;native")

    if(TARGET physics2d_simulation)
    add_executable(plugin_library_test test/plugin.cpp)
    target_link_libraries(plugin_library_test PRIVATE editor_extensions scene_control_api lux::engine::simulation::simulation_composition)

    if(MSVC)
        target_compile_options(plugin_library_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(plugin_library_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME plugin.physics2d COMMAND plugin_library_test "${CMAKE_BINARY_DIR}/share/lux-engine/plugins/catalog.json" "${CMAKE_BINARY_DIR}")
    set_tests_properties(plugin.physics2d PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "editor;native")

        add_dependencies(plugin_library_test physics2d_simulation builtin_runtime_plugin lux_plugin_capabilities physics2d_editor)
    endif()
endif()

