cmake_minimum_required(VERSION 3.22)
project(lux_script_static_ability_consumer LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-function REQUIRED COMPONENTS script_core)

include_component_cmake_scripts(lux::engine::function::script_core)
if(NOT COMMAND lux_script_abilities)
    get_target_property(script_core_assets lux::engine::function::script_core IMPORTED_ASSETS_PREFIX)
    include("${script_core_assets}/cmake_scripts/engine_script_ability_codegen.cmake")
endif()

add_executable(lux_script_static_ability_consumer main.cpp)
target_include_directories(lux_script_static_ability_consumer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(lux_script_static_ability_consumer PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(lux_script_static_ability_consumer PRIVATE /UNDEBUG)
else()
    target_compile_options(lux_script_static_ability_consumer PRIVATE -UNDEBUG)
endif()
target_link_libraries(lux_script_static_ability_consumer PRIVATE lux::engine::function::script_core)
lux_script_abilities(
    TARGET lux_script_static_ability_consumer
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/StaticAbility.hpp
    LOGICAL_PATHS StaticAbility.hpp
)
