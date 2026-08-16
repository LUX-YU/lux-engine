generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL3D_NAVIGATION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL3D_NAVIGATION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_SPATIAL3D_NAVIGATION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/spatial3d/navigation/visibility.h
)

add_component(
    COMPONENT_NAME runtime_spatial3d_navigation_adapter
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_spatial3d_navigation_adapter
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Navigation3DPrepareService.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Spatial3DNavigationAdapterSystem.cpp
)

component_include_directories(
    runtime_spatial3d_navigation_adapter
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    runtime_spatial3d_navigation_adapter
    PRIVATE LUX_ENGINE_RUNTIME_SPATIAL3D_NAVIGATION_LIBRARY
)

target_link_libraries(
    runtime_spatial3d_navigation_adapter
    PUBLIC
        lux::engine::runtime::runtime_execution
        lux::engine::runtime::runtime_entity_scene
        lux::engine::ecs::navigation3d
        lux::engine::function::navigation_detour3d
)

component_add_transitive_commands(
    runtime_spatial3d_navigation_adapter
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_execution runtime_entity_scene)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS navigation3d)"
    "find_package(lux-engine-function REQUIRED COMPONENTS navigation_detour3d)"
)

lux_classify_target(
    TARGET  runtime_spatial3d_navigation_adapter
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

add_executable(
    spatial3d_navigation_adapter_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/spatial3d_navigation_adapter_test.cpp
)
target_link_libraries(
    spatial3d_navigation_adapter_test
    PRIVATE
        lux::engine::runtime::runtime_spatial3d_navigation_adapter
        lux::engine::resource::entity_scene
)
if(MSVC)
    target_compile_options(spatial3d_navigation_adapter_test PRIVATE /UNDEBUG)
else()
    target_compile_options(spatial3d_navigation_adapter_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  spatial3d_navigation_adapter_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(
    NAME spatial3d_navigation_adapter_test
    COMMAND spatial3d_navigation_adapter_test
)
set_tests_properties(
    spatial3d_navigation_adapter_test
    PROPERTIES
        LABELS "cpu;runtime;spatial3d;entity_scene;navigation;async"
        TIMEOUT 60
)
