project(lux-engine)
cmake_minimum_required(VERSION 3.16)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RELEASE")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_TOOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tools)
set(PROJECT_NAMESPACE lux::engine)

set(THIRD_PARTY_PKG_DIR			${CMAKE_CURRENT_SOURCE_DIR}/prebuild)
set(THIRD_PARTY_SOURCE_DIR		${CMAKE_CURRENT_SOURCE_DIR}/prebuild/source)
set(THIRD_PARTY_PKG_INSTALL_DIR ${THIRD_PARTY_PKG_DIR}/install)
list(APPEND CMAKE_PREFIX_PATH 	${THIRD_PARTY_PKG_INSTALL_DIR})

include(${CMAKE_TOOL_DIR}/subdirectory_list.cmake)
include(${CMAKE_TOOL_DIR}/add_tool_module.cmake)
include(CMakePackageConfigHelpers)
set(lux_engine_layers "platform" "core" "resource" "function" "tools")

foreach(layer ${lux_engine_layers})
    set(${layer}_config_targets_file "lux-engine-${layer}-config-targets.cmake")
    add_subdirectory(modules/${layer})
endforeach()

# ---------------------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------------------
set(LUX_ENGINE_VERSION 0.0.1)

configure_package_config_file(
	  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/lux-engine-config.cmake.in
	  ${CMAKE_CURRENT_BINARY_DIR}/lux-engine-config.cmake
	  INSTALL_DESTINATION ${CMAKE_INSTALL_PREFIX}/share/lux-engine
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/lux-engine-config-version.cmake
    VERSION ${LUX_ENGINE_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
	FILES ${CMAKE_CURRENT_BINARY_DIR}/lux-engine-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/lux-engine-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_PREFIX}/share/lux-engine
)
