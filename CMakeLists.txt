cmake_minimum_required(VERSION 3.18)
project(lux-engine)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RELEASE")
endif()

if(WIN32) 
    set(CMAKE_DEBUG_POSTFIX d)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_TOOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tools)
set(PROJECT_NAMESPACE lux::engine)

set(ENABLE_SUPERBUILD OFF CACHE BOOL "Prebuild the dependencies.")

if(ENABLE_SUPERBUILD)
    set(THIRD_PARTY_PKG_DIR			${CMAKE_CURRENT_SOURCE_DIR}/prebuild)
    set(THIRD_PARTY_SOURCE_DIR		${CMAKE_CURRENT_SOURCE_DIR}/prebuild/source)
    set(THIRD_PARTY_PKG_INSTALL_DIR ${THIRD_PARTY_PKG_DIR}/install)
    list(APPEND CMAKE_PREFIX_PATH 	${THIRD_PARTY_PKG_INSTALL_DIR})
endif()

# load the build system helper
find_package(lux-cmake-toolset CONFIG REQUIRED)

set(lux_engine_layers "platform" "core" "resource" "function" "tools")

foreach(layer ${lux_engine_layers})
    add_subdirectory(modules/${layer})
endforeach()
