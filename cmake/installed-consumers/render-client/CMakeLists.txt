cmake_minimum_required(VERSION 3.28)

project(render_client_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan ON)
set(CMAKE_DISABLE_FIND_PACKAGE_NRI ON)
set(CMAKE_DISABLE_FIND_PACKAGE_stdexec ON)

find_package(lux-engine-platform CONFIG REQUIRED COMPONENTS platform_ipc)
find_package(lux-cxx CONFIG REQUIRED COMPONENTS core container)
find_package(lux-engine-resource CONFIG REQUIRED COMPONENTS description_types)
find_package(lux-engine-core CONFIG REQUIRED COMPONENTS error_values)

# Only the test source is shared. Its includes and link providers come from the SDK;
# the synchronous declaration/IPC contract needs neither a Server nor an execution backend.
add_executable(render_client "${CMAKE_CURRENT_SOURCE_DIR}/../../../modules/function/render/test/SyncClient.cpp")

# SurfaceDescription uses NRI's platform-neutral Window value, but needs no NRI binary or backend lookup.
get_filename_component(sdk_prefix "${lux-engine-platform_DIR}/../.." ABSOLUTE)
target_include_directories(render_client SYSTEM PRIVATE "${sdk_prefix}/include/lux/engine/render/nri")

target_link_libraries(
    render_client
    PRIVATE
        lux::engine::platform::platform_ipc
        lux::engine::resource::description_types
        lux::engine::core::error_values
        lux::cxx::core
        lux::cxx::container
)

if(MSVC)
    target_compile_options(render_client PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR-)
endif()

enable_testing()
add_test(NAME installed.render.sync_client COMMAND render_client)
