# Find EnTT
find_package(EnTT    CONFIG REQUIRED)
find_path(ENTT_INCLUDE_DIRS "entt/entt.hpp")
if(NOT ENTT_INCLUDE_DIRS)
    message(FATAL_ERROR "EnTT include directory not found.")
else()
    message(STATUS "EnTT include directory found: ${ENTT_INCLUDE_DIRS}")
endif()

# Find LuaJIT
find_package(LuaJIT REQUIRED)

# Find LuaJIT include directory
find_path(SOL2_INCLUDE_DIRS "sol/abort.hpp" REQUIRED)

set(LUX_META_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LuxObject.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Meta.cpp
)

add_component(
    COMPONENT_NAME  meta
    NAMESPACE       lux::engine::core
    OUTPUT_NAME     lux_engine_core_meta
    SOURCE_FILES    ${LUX_META_SRCS}
)

component_include_directories(
    meta
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED   ${ENTT_INCLUDE_DIRS}
                        ${SOL2_INCLUDE_DIRS}
                        ${LUAJIT_INCLUDE_DIRS}
    INSTALL_TIME        include
    PRIVATE             pinclude
)

target_compile_definitions(
    meta
    PRIVATE
    LUX_CORE_LIBRARY
)

component_add_transitive_commands(
    meta
    "find_package(lux-cxx REQUIRED COMPONENTS reflection_generator)"
    "find_package(EnTT    CONFIG REQUIRED)"
)

component_add_cmake_scripts(
	meta
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_add_meta.cmake
)

target_link_libraries(
    meta
    PUBLIC
    lux::cxx::reflection_generator
    EnTT::EnTT
    PRIVATE
    LuaJIT::LuaJIT
)
