cmake_minimum_required(VERSION 3.22)
project(lux_script_binding_authoring_consumer LANGUAGES CXX)

find_package(
    lux-engine-authoring
    REQUIRED COMPONENTS authoring_script_binding
)
find_package(
    lux-engine-simulation
    REQUIRED COMPONENTS simulation_description
)
find_package(
    lux-engine-simulation-asset
    REQUIRED COMPONENTS simulation_asset
)

add_executable(lux_script_binding_authoring_consumer main.cpp)
target_compile_features(
    lux_script_binding_authoring_consumer PRIVATE cxx_std_20
)
if(MSVC)
    target_compile_options(
        lux_script_binding_authoring_consumer PRIVATE /UNDEBUG
    )
else()
    target_compile_options(
        lux_script_binding_authoring_consumer PRIVATE -UNDEBUG
    )
endif()
target_link_libraries(
    lux_script_binding_authoring_consumer
    PRIVATE
        lux::engine::authoring::authoring_script_binding
        lux::engine::simulation::description
        lux::engine::simulation::asset
)
