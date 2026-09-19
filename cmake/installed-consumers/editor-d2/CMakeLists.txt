cmake_minimum_required(VERSION 3.24)
project(lux_editor_d2_consumer LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    message(FATAL_ERROR "D2 qualification requires RelWithDebInfo")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(LUX_GENERATE_HEADER_DIR "${CMAKE_BINARY_DIR}/generated")

find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS reflection_generator)
find_package(lux-engine-core REQUIRED COMPONENTS meta serialization)
find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS schema)
find_package(lux-engine-editor-scene REQUIRED COMPONENTS editor_scene editor_scene_ui)
find_package(lux-engine-editor-material REQUIRED COMPONENTS editor_material editor_material_ui)
find_package(lux-engine-editor-flowforge REQUIRED COMPONENTS editor_flowforge editor_flowforge_ui)
find_package(lux-engine-editor-assets REQUIRED COMPONENTS editor_assets)
if(TARGET assimp::assimp)
    set_property(TARGET assimp::assimp PROPERTY MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
endif()
include_component_cmake_scripts(lux::engine::core::meta)
include_component_cmake_scripts(lux::engine::simulation::ecs::schema)
include_component_cmake_scripts(lux::engine::editor::editor_scene_ui)

add_library(consumer_domain SHARED Domain.cpp)
target_compile_definitions(consumer_domain PRIVATE CONSUMER_DOMAIN_LIBRARY)
target_include_directories(consumer_domain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${LUX_GENERATE_HEADER_DIR}")
target_link_libraries(consumer_domain PUBLIC lux::engine::simulation::ecs::schema)
engine_target_add_ecs_component_codegen(NAME consumer_schema TARGET consumer_domain
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Domain.cpp"
    HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/consumer/Component.hpp"
    LOGICAL_PATH consumer/Component.hpp SYMBOL Consumer)

add_library(consumer_gui SHARED Gui.cpp GuiGesture.cpp)
set_property(TARGET consumer_gui PROPERTY LUX_ARCH_LAYER EDITOR)
target_compile_definitions(consumer_gui PRIVATE CONSUMER_GUI_LIBRARY)
target_link_libraries(consumer_gui PUBLIC consumer_domain lux::engine::editor::editor_scene_ui)
engine_target_add_imgui_inspector_codegen(NAME consumer TARGET consumer_gui
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Gui.cpp"
    HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/consumer/Component.hpp"
    LOGICAL_PATH consumer/Component.hpp COMPONENTS consumer::Component)
add_dependencies(consumer_gui consumer_domain)

add_executable(consumer_protocol main.cpp SceneWorkflow.cpp)
target_link_libraries(consumer_protocol PRIVATE consumer_domain consumer_gui)
option(D2_MEASURE_COMPONENT_COPIES "Instrument only the test component, never normal timing artifacts" OFF)
set(D2_EXPECT_ADOPT_COPIES 1 CACHE STRING "One after-value capture at field completion")
foreach(target consumer_domain consumer_gui consumer_protocol)
    if(D2_MEASURE_COMPONENT_COPIES)
        target_compile_definitions(${target} PRIVATE CONSUMER_MEASURE_COPIES
            CONSUMER_EXPECT_ADOPT_COPIES=${D2_EXPECT_ADOPT_COPIES})
        if(D2_EXPECT_ADOPT_COPIES STREQUAL "1")
            target_compile_definitions(${target} PRIVATE CONSUMER_WIDGET_DIAGNOSTICS)
        endif()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /UNDEBUG /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
endforeach()

enable_testing()
add_test(NAME editor.d2.installed_component COMMAND consumer_protocol "${CMAKE_BINARY_DIR}/project")

option(D2_BUILD_PROTOCOL_TESTS "Build the real document and Process protocol consumers" ON)
if(D2_BUILD_PROTOCOL_TESTS)
    add_subdirectory(protocol)
endif()
