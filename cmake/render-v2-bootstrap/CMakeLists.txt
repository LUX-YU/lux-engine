cmake_minimum_required(VERSION 3.25)

project(lux-render-v2-bootstrap VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /GR- /we4541)
else()
    add_compile_options(-fno-rtti)
endif()

include(CTest)


find_package(lux-cmake-toolset CONFIG REQUIRED)

include("${CMAKE_CURRENT_LIST_DIR}/../InstallComponents.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../TargetClassification.cmake")
find_package(lux-cxx CONFIG REQUIRED COMPONENTS compile_time)

include("${CMAKE_CURRENT_LIST_DIR}/../../modules/resource/description/cmake/ValueTypes.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../modules/core/error/cmake/Values.cmake")



include("${CMAKE_CURRENT_LIST_DIR}/../../modules/resource/description/cmake/ShaderMetadata.cmake")

set(platform_components platform_ipc gapi platform_process)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/platform/process" platform-process)

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_CORE_LIBRARY
    PUBLIC_MACRO_NAME LUX_CORE_PUBLIC
    GENERATE_FILE_PATH lux/engine/core/visibility.h
)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/core/async_port" async-port)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/core/error" core-error)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/core/log" core-log)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/core/execution" core-execution)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/core/task" core-task)
set_target_properties(task PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

if(BUILD_TESTING)
    # Real host scheduler consumers. These dependencies never enter Render production targets.
    find_package(lux-cxx CONFIG REQUIRED COMPONENTS core algorithm compile_time container)

    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/platform/dynamic_library" dynamic-library)

    foreach(target dynamic_library error core_execution error_registry_test execution_error_test
        task_scope_test port_sender_test execution_run_loop_test
    )
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
    endforeach()

    list(APPEND platform_components dynamic_library)

endif()

# Existing authoring/lowering algorithms are only build-time asset dependencies.
if(BUILD_TESTING)
    include("${CMAKE_CURRENT_LIST_DIR}/../../modules/resource/description/cmake/ValueTypes.cmake")
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/core/object" core-object)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/resource/identity" resource-identity)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/function/graph" function-graph)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/function/material" material-graph)

    # Build the real Cooked material compiler once; it is a tool/test dependency, never a Render dependency.
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../engine/toolchain/shader" shader-toolchain)
    set(LUX_MATERIAL_BUILD_COOKER OFF)
    set(LUX_EDITOR_BUILD_TOOLCHAIN_TESTS ON)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../engine/toolchain/material" material-toolchain)
    set_target_properties(toolchain_material_compiler lux_shader_emitter material_domain_compiler_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin"
    )
    set_target_properties(lux_shader_metadata PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/render")

    foreach(target object object_ownership_plugin object_ownership_test object_tree_test object_queue_test
        object_target_test graph graph_edit_test material_graph material_registered_graph_test
        material_authoring_schema_test material_builtin_nodes_test material_math_registration_test
        material_lowering_test material_node_identity_test material_shader_ir_test material_node_catalog_test
        material_node_plugin material_node_plugin_test
    )
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
    endforeach()

endif()

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/function/render" render)

# Real application asset transport consumers, with their source dependency closure.
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/core/math" core-math)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/core/type_info" core-type-info)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/core/serialization" core-serialization)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/core/semantic" core-semantic)
generate_visibility_header(
    ENABLE_MACRO_NAME LUX_FUNCTION_LIBRARY
    PUBLIC_MACRO_NAME LUX_FUNCTION_PUBLIC
    GENERATE_FILE_PATH lux/engine/function/visibility.h
)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/function/script/core" script-core)
if(NOT TARGET identity)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/resource/identity" resource-identity)
endif()
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/resource/description" resource-description)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/resource/asset" resource-asset)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../engine/process/asset_loading" process-asset-loading)
if(BUILD_TESTING AND WIN32)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/platform/window" platform-window)
    list(APPEND platform_components window)
endif()
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../engine/rendering" engine-rendering)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/platform/ipc" ipc)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/platform/gapi" gapi)

set(resource_components description_types shader_metadata description asset identity)
if(BUILD_TESTING)
    # identity is also an Engine asset transport dependency.
endif()

lux_engine_install_components(
    PROJECT_NAME lux-engine-resource
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::resource
    COMPONENTS ${resource_components}
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-platform
    VERSION 0.0.1
    NAMESPACE lux::engine::platform
    COMPONENTS ${platform_components}
)

set(core_components async_port error error_values log task math type_info serialization semantic)
if(BUILD_TESTING)
    list(APPEND core_components object)
endif()

lux_engine_install_components(
    PROJECT_NAME lux-engine-core
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::core
    COMPONENTS ${core_components}
)

set(function_components script_core)
if(BUILD_TESTING)
    list(APPEND function_components graph material_graph)
endif()

lux_engine_install_components(
    PROJECT_NAME lux-engine-function
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::function
    COMPONENTS ${function_components}
)
