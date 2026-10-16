cmake_minimum_required(VERSION 3.22)

project(lux-render-v2-bootstrap VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /GR- /we4541)
else()
    add_compile_options(-fno-rtti)
endif()

include(CTest)

option(LUX_RENDER_CLIENT_ONLY "Build only the synchronous Client and its dependencies" OFF)

find_package(lux-cmake-toolset CONFIG REQUIRED)

include("${CMAKE_CURRENT_LIST_DIR}/../InstallComponents.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../TargetClassification.cmake")
find_package(lux-cxx CONFIG REQUIRED COMPONENTS compile_time)

if(LUX_RENDER_CLIENT_ONLY)
    # This is a real source build of the synchronous SDK, with no Native or async component selected.
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/platform/ipc" ipc)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/function/render" render)

    lux_engine_install_components(
        PROJECT_NAME lux-engine-platform
        VERSION ${PROJECT_VERSION}
        NAMESPACE lux::engine::platform
        COMPONENTS platform_ipc
    )
    return()
endif()

find_package(stdexec CONFIG REQUIRED)

set(platform_components platform_ipc gapi platform_process)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/platform/process" platform-process)

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_CORE_LIBRARY
    PUBLIC_MACRO_NAME LUX_CORE_PUBLIC
    GENERATE_FILE_PATH lux/engine/core/visibility.h
)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/core/async_port" async-port)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/core/error" core-error)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/core/execution" core-execution)

if(BUILD_TESTING)
    # Real host scheduler consumers. These dependencies never enter Render production targets.
    find_package(lux-cxx CONFIG REQUIRED COMPONENTS core algorithm compile_time container)

    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/platform/dynamic_library" dynamic-library)

    foreach(target dynamic_library error core_execution error_registry_test execution_error_test
        task_scope_test port_sender_test
    )
        set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
    endforeach()

    list(APPEND platform_components dynamic_library)

endif()

# The user removed the old V2 source and its tests. Historical F4/F5/F6 scripts
# remain read-only references; only implemented rebuild stages enter this build.
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/function/render" render)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/platform/ipc" ipc)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/platform/gapi" gapi)

lux_engine_install_components(
    PROJECT_NAME lux-engine-platform
    VERSION 0.0.1
    NAMESPACE lux::engine::platform
    COMPONENTS ${platform_components}
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-core
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::core
    COMPONENTS async_port error
)
