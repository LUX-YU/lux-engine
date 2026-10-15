cmake_minimum_required(VERSION 3.22)

project(lux-render-v2-bootstrap LANGUAGES CXX)

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

find_package(lux-cmake-toolset CONFIG REQUIRED)

include("${CMAKE_CURRENT_LIST_DIR}/../InstallComponents.cmake")

# The user removed the old V2 source and its tests. Historical F4/F5/F6 scripts
# remain read-only references; only implemented rebuild stages enter this build.
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/function/render" render)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/platform/ipc" ipc)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/platform/gapi" gapi)

lux_engine_install_components(
    PROJECT_NAME lux-engine-platform
    VERSION 0.0.1
    NAMESPACE lux::engine::platform
    COMPONENTS platform_ipc gapi
)
