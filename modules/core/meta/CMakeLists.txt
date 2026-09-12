# Runtime metadata uses the reflection vocabulary and generic containers, but
# never the libclang-backed generator library.  The generator is consumed only
# by engine_add_meta.cmake as a BUILD_TOOL.
find_package(
    lux-cxx
    CONFIG REQUIRED
    COMPONENTS algorithm compile_time container reflection_runtime
)

set(LUX_META_SRCS
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
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time container reflection_runtime)"
)

component_add_cmake_scripts(
	meta
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_type_runtime_info.cmake
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_add_type_info.cmake
)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_type_runtime_info.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_add_type_info.cmake)

install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/template/
    DESTINATION share/lux-engine-core/meta/template
)

target_link_libraries(
    meta
    PUBLIC
    lux::cxx::algorithm
    lux::cxx::compile_time
    lux::cxx::container
    lux::cxx::reflection_runtime
)

component_add_internal_dependencies(meta type_info)
