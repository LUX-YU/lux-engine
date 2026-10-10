cmake_minimum_required(VERSION 3.22)

project(lux-render-v2-bootstrap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

# R0 has no production targets. R1 adds only the real render_core target.
