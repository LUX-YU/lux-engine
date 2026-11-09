generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/scene/visibility.h
)

add_component(
    COMPONENT_NAME scene_composition
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_composition
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneInstance.cpp
)

component_include_directories(
    scene_composition
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_include_directories(scene_composition PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
target_compile_definitions(scene_composition PRIVATE LUX_ENGINE_SCENE_LIBRARY)
target_link_libraries(
    scene_composition
    PUBLIC
        lux::engine::world::world_composition
        lux::engine::simulation::composition
        lux::engine::scene::scene_system
)
component_add_transitive_commands(
    scene_composition
    "find_package(lux-engine-world-composition REQUIRED COMPONENTS world_composition)"
    "find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)"
    "find_package(lux-engine-scene-system REQUIRED COMPONENTS scene_system)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_composition
)

if(BUILD_TESTING)
    add_executable(scene_driver_test test/driver.cpp test/transform.cpp)
    target_include_directories(scene_driver_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
    target_link_libraries(scene_driver_test PRIVATE scene_composition lux::engine::scene::system_transform)
    if(MSVC)
        target_compile_options(scene_driver_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(scene_driver_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME scene.driver COMMAND scene_driver_test)
    add_executable(scene_runtime_test test/runtime.cpp)
    target_link_libraries(scene_runtime_test PRIVATE scene_composition lux::engine::scene::system_transform)
    if(MSVC)
        target_compile_options(scene_runtime_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(scene_runtime_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME scene.runtime COMMAND scene_runtime_test)

endif()
