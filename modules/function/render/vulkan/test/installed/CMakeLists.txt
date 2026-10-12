cmake_minimum_required(VERSION 3.22)
project(RenderBindingInstalledConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(RenderBinding CONFIG REQUIRED)
add_executable(consumer Main.cpp)

target_link_libraries(consumer PRIVATE lux::engine::render::render_vulkan_graph)
