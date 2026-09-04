cmake_minimum_required(VERSION 3.22)
project(lux_script_ability_ipo_consumer LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error LANGUAGES CXX)
if(NOT ipo_supported)
    message(FATAL_ERROR "IPO is required for this installed consumer: ${ipo_error}")
endif()

find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-function REQUIRED COMPONENTS script_core script_native)
find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script_native)

include_component_cmake_scripts(lux::engine::function::script_core)
if(NOT COMMAND lux_script_abilities)
    get_target_property(script_core_assets lux::engine::function::script_core IMPORTED_ASSETS_PREFIX)
    include("${script_core_assets}/cmake_scripts/engine_script_ability_codegen.cmake")
endif()

add_executable(lux_script_ability_ipo_consumer main.cpp)
set_property(TARGET lux_script_ability_ipo_consumer PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
target_compile_features(lux_script_ability_ipo_consumer PRIVATE cxx_std_20)
target_include_directories(lux_script_ability_ipo_consumer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
    lux_script_ability_ipo_consumer
    PRIVATE
        lux::engine::function::script_core
        lux::engine::function::script_native
        lux::engine::simulation::simulation_script_native
)
lux_script_abilities(
    TARGET lux_script_ability_ipo_consumer
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/TinyAbility.hpp
    LOGICAL_PATHS TinyAbility.hpp
)
