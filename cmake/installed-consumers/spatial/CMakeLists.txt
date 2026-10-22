cmake_minimum_required(VERSION 3.24)
project(lux_spatial_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
find_package(lux-engine-scene-mesh-query REQUIRED COMPONENTS scene_mesh_query)
if(TARGET lux::engine::function::render OR TARGET lux::engine::scene::scene_render)
    message(FATAL_ERROR "CPU spatial query imported Renderer")
endif()

add_executable(query_consumer query.cpp)
target_link_libraries(query_consumer PRIVATE lux::engine::scene::scene_mesh_query)

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render)
add_library(probe_feature SHARED Probe.cpp)
target_include_directories(probe_feature
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_definitions(probe_feature PRIVATE PROBE_LIBRARY)
target_link_libraries(probe_feature PUBLIC lux::engine::function::render)

add_executable(feature_consumer feature.cpp)
target_link_libraries(feature_consumer PRIVATE probe_feature)

foreach(target probe_feature feature_consumer query_consumer)
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG /utf-8 /Zc:__cplusplus /Zc:preprocessor /GR- /we4541)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG -fno-rtti)
    endif()
endforeach()

enable_testing()
add_test(NAME spatial.external_feature COMMAND feature_consumer)
add_test(NAME spatial.query_without_renderer COMMAND query_consumer)
