cmake_minimum_required(VERSION 3.22)

project(lux-render-v2-bootstrap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(lux-cmake-toolset CONFIG REQUIRED)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../modules/function/render/core" render_core)
