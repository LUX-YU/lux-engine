cmake_minimum_required(VERSION 3.24)
project(window_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(lux-engine-platform REQUIRED COMPONENTS window)

add_executable(window_consumer main.cpp)
target_link_libraries(window_consumer PRIVATE lux::engine::platform::window)
if(MSVC)
    target_compile_options(window_consumer PRIVATE /UNDEBUG /permissive- /utf-8 /GR- /we4541)
else()
    target_compile_options(window_consumer PRIVATE -UNDEBUG -fno-rtti)
endif()

enable_testing()
add_test(NAME installed.window.lifecycle COMMAND window_consumer)
set_tests_properties(installed.window.lifecycle PROPERTIES TIMEOUT 30 LABELS desktop)
