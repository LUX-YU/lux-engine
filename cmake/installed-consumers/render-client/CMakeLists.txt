cmake_minimum_required(VERSION 3.28)

project(render_client_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan ON)
set(CMAKE_DISABLE_FIND_PACKAGE_NRI ON)
set(CMAKE_DISABLE_FIND_PACKAGE_stdexec ON)

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render)

# Only the test source is shared. All headers and binary dependencies must come from the SDK.
add_executable(render_client "${CMAKE_CURRENT_SOURCE_DIR}/../../../modules/function/render/test/SyncClient.cpp")
target_link_libraries(render_client PRIVATE lux::engine::function::render)

if(MSVC)
    target_compile_options(render_client PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR-)
endif()

enable_testing()
add_test(NAME installed.render.sync_client COMMAND render_client)
