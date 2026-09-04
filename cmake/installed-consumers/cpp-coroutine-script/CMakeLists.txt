cmake_minimum_required(VERSION 3.22)
project(lux_cpp_coroutine_script_consumer LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-core REQUIRED COMPONENTS meta)
find_package(lux-engine-function REQUIRED COMPONENTS script_artifact script_core)
find_package(
    lux-engine-simulation
    REQUIRED COMPONENTS
        simulation_script
        simulation_scripting_core
        simulation_script_cpp_static
)

include_component_cmake_scripts(lux::engine::core::meta)
if(NOT COMMAND engine_add_type_runtime_info)
    get_target_property(installed_meta_assets lux::engine::core::meta IMPORTED_ASSETS_PREFIX)
    include("${installed_meta_assets}/cmake_scripts/engine_type_runtime_info.cmake")
endif()
engine_add_type_runtime_info(
    NAME installed_cpp_coroutine_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/ConsumerBehavior.hpp
    REGISTER_FUNC_NAME lux_installed_cpp_coroutine
)

add_executable(lux_cpp_coroutine_script_consumer main.cpp)
target_include_directories(lux_cpp_coroutine_script_consumer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(lux_cpp_coroutine_script_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_cpp_coroutine_script_consumer
    PRIVATE
        lux::engine::core::meta
        lux::engine::function::script_artifact
        lux::engine::simulation::simulation_script_cpp_static
)
engine_target_add_type_runtime_info(
    TARGET lux_cpp_coroutine_script_consumer
    METAS installed_cpp_coroutine_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
)
