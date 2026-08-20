find_package(stduuid CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL3D_PHYSICS_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL3D_PHYSICS_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_SPATIAL3D_PHYSICS_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/spatial3d/physics/visibility.h
)

add_component(
    COMPONENT_NAME runtime_spatial3d_physics
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_spatial3d_physics
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/StaticCollider3DBinding.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/StaticCollider3DPrepareService.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/StaticCollider3DSystem.cpp
)

component_include_directories(
    runtime_spatial3d_physics
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    runtime_spatial3d_physics
    PRIVATE LUX_ENGINE_RUNTIME_SPATIAL3D_PHYSICS_LIBRARY
)

target_link_libraries(
    runtime_spatial3d_physics
    PUBLIC
        lux::engine::runtime::runtime_entity_scene
        lux::engine::runtime::runtime_execution
        lux::engine::ecs::physics3d
        lux::engine::ecs::scene_format
        lux::engine::resource::asset
)

component_add_transitive_commands(
    runtime_spatial3d_physics
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_entity_scene runtime_execution)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS physics3d scene_format)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
)

lux_classify_target(
    TARGET  runtime_spatial3d_physics
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

add_executable(
    static_collider3d_system_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/static_collider3d_system_test.cpp
)
target_link_libraries(
    static_collider3d_system_test
    PRIVATE
        lux::engine::runtime::runtime_spatial3d_physics
        stduuid
)
if(MSVC)
    target_compile_options(static_collider3d_system_test PRIVATE /UNDEBUG)
else()
    target_compile_options(static_collider3d_system_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  static_collider3d_system_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    INTEGRATION
)
