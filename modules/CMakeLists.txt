include(${CMAKE_TOOL_DIR}/add_tool_module.cmake)
list(APPEND CMAKE_PREFIX_PATH ${THIRD_PARTY_PKG_INSTALL_DIR})

add_subdirectory(platform)
add_subdirectory(core)
add_subdirectory(resource)
add_subdirectory(function)
add_subdirectory(tools)
