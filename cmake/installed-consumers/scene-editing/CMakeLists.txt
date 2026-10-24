cmake_minimum_required(VERSION 3.25)

project(scene_editing_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_DISABLE_FIND_PACKAGE_Vulkan TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_NRI TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_glfw3 TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_imgui TRUE)

find_package(lux-engine-context REQUIRED COMPONENTS engine_context)
find_package(lux-engine-editor-framework REQUIRED COMPONENTS lux_editor_scene_document lux_editor_scene)
find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS transform)
find_package(lux-engine-process-world-loading REQUIRED COMPONENTS process_world_loading)

add_executable(scene_editing main.cpp)

target_link_libraries(scene_editing PRIVATE
    lux::engine::engine_context
    lux::engine::editor::lux_editor_scene_document
    lux::engine::editor::lux_editor_scene
    lux::engine::simulation::ecs::transform
    lux::engine::process::process_world_loading
)

if(MSVC)
    target_compile_options(scene_editing PRIVATE /GR- /UNDEBUG /utf-8)
else()
    target_compile_options(scene_editing PRIVATE -fno-rtti -UNDEBUG)
endif()

enable_testing()

add_test(NAME installed.scene_editing COMMAND scene_editing "${CMAKE_CURRENT_BINARY_DIR}/author.luxscene")
set_tests_properties(installed.scene_editing PROPERTIES TIMEOUT 20)
