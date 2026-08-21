cmake_minimum_required(VERSION 3.24)
project(lux_script_native_installed_consumer LANGUAGES CXX)

find_package(lux-engine-function REQUIRED COMPONENTS script_native)

if(NOT DEFINED LUX_SCRIPT_NATIVE_FIXTURE)
    message(FATAL_ERROR "LUX_SCRIPT_NATIVE_FIXTURE is required")
endif()
file(TO_CMAKE_PATH
    "${LUX_SCRIPT_NATIVE_FIXTURE}"
    LUX_SCRIPT_NATIVE_FIXTURE_CMAKE_PATH
)

add_executable(script_native_installed_consumer main.cpp)
target_compile_features(script_native_installed_consumer PRIVATE cxx_std_20)
target_compile_definitions(
    script_native_installed_consumer
    PRIVATE
        LUX_SCRIPT_NATIVE_FIXTURE="${LUX_SCRIPT_NATIVE_FIXTURE_CMAKE_PATH}"
)
target_link_libraries(
    script_native_installed_consumer
    PRIVATE
        lux::engine::function::script_native
)
