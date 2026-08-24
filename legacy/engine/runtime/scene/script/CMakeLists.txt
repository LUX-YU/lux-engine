generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_RUNTIME_SCENE_SCRIPT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_RUNTIME_SCENE_SCRIPT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_RUNTIME_SCENE_SCRIPT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/scene/script/visibility.h
)

add_component(
    COMPONENT_NAME runtime_scene_script
    NAMESPACE lux::engine::runtime
    OUTPUT_NAME lux_engine_runtime_scene_script
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneScriptRuntime.cpp
)

component_include_directories(
    runtime_scene_script
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    runtime_scene_script
    PRIVATE LUX_RUNTIME_SCENE_SCRIPT_LIBRARY
)

target_link_libraries(
    runtime_scene_script
    PUBLIC
        lux::engine::ecs::core
        lux::engine::ecs::scripting
        lux::engine::function::input
        lux::engine::resource::asset
        lux::engine::runtime::runtime_assets
    PRIVATE
        lux::engine::core::log
)

lux_classify_target(
    TARGET  runtime_scene_script
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

component_add_transitive_commands(
    runtime_scene_script
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core scripting)"
    "find_package(lux-engine-function REQUIRED COMPONENTS input)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_assets)"
)

if(ENABLE_EXECUTION_TEST)
    add_executable(
        script_asset_request_system_test
        ${CMAKE_CURRENT_SOURCE_DIR}/test/script_asset_request_system_test.cpp
    )
    target_link_libraries(
        script_asset_request_system_test
        PRIVATE runtime_scene_script
    )
endif()
