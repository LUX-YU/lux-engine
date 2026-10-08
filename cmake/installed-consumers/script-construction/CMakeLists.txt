cmake_minimum_required(VERSION 3.24)
project(script_construction_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script_cpp_static simulation_script_lua)
find_package(LuxLua55 5.5.1 EXACT CONFIG REQUIRED)

add_executable(cpp_script_construction cpp.cpp)
target_link_libraries(cpp_script_construction PRIVATE lux::engine::simulation::simulation_script_cpp_static)

add_executable(lua_script_construction lua.cpp)
target_link_libraries(lua_script_construction PRIVATE lux::engine::simulation::simulation_script_lua LuxLua55::Runtime)

foreach(target IN ITEMS cpp_script_construction lua_script_construction)
    if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG /permissive- /utf-8 /GR- /we4541)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG -fno-rtti)
    endif()
endforeach()

enable_testing()
add_test(NAME installed.script.cpp_construction COMMAND cpp_script_construction)
add_test(NAME installed.script.lua_construction COMMAND lua_script_construction)
set_tests_properties(installed.script.cpp_construction installed.script.lua_construction PROPERTIES TIMEOUT 30)
