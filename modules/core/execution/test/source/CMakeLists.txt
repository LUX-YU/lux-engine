cmake_minimum_required(VERSION 3.25)

project(ExecutionSourceConsumer VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /GR- /we4541)
endif()

find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-cxx CONFIG REQUIRED COMPONENTS compile_time)

include("${LUX_SOURCE_DIR}/cmake/InstallComponents.cmake")

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_CORE_LIBRARY
    PUBLIC_MACRO_NAME LUX_CORE_PUBLIC
    GENERATE_FILE_PATH lux/engine/core/visibility.h
)

add_subdirectory("${LUX_SOURCE_DIR}/modules/core/async_port" async-port)
add_subdirectory("${LUX_SOURCE_DIR}/modules/core/error" error)
add_subdirectory("${LUX_SOURCE_DIR}/modules/core/execution" execution)

lux_engine_install_components(
    PROJECT_NAME lux-engine-core
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::core
    COMPONENTS async_port error
)
