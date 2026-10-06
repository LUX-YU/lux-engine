cmake_minimum_required(VERSION 3.24)
project(ui_composition_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(lux-engine-function REQUIRED COMPONENTS ui)

# Compile the same behavioral cases solely against installed public providers.
# No production source or private include path is added to this consumer.
get_filename_component(engine_source "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(ui_composition "${engine_source}/modules/function/ui/test/composition.cpp")
target_link_libraries(ui_composition PRIVATE lux::engine::function::ui)
if(MSVC)
    target_compile_options(ui_composition PRIVATE /UNDEBUG /permissive- /utf-8 /GR- /we4541)
else()
    target_compile_options(ui_composition PRIVATE -UNDEBUG -fno-rtti)
endif()
enable_testing()
add_test(NAME installed.ui.composition COMMAND ui_composition)
