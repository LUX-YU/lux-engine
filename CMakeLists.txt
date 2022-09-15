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

include(${CMAKE_TOOL_DIR}/subdirectory_list.cmake)
add_subdirectory(modules)

option(ENABLE_PLAYGROUND ON)
if(ENABLE_PLAYGROUND)
	add_subdirectory(playground)
endif()

# ---------------------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

set(LIB_INSTALL_DIR lib/)
set(config_targets_file "lux-engine-config-targets.cmake")

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
	EXPORT		lux::engine
	DESTINATION ${CMAKE_INSTALL_PREFIX}/share/lux-engine
	NAMESPACE   ${PROJECT_NAMESPACE}::
	FILE		${config_targets_file}
)

install(
	FILES ${CMAKE_CURRENT_BINARY_DIR}/lux-engine-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/lux-engine-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_PREFIX}/share/lux-engine
)
