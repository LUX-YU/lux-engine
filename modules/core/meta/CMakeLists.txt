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

if(BUILD_TESTING)
    find_package(lux-cxx CONFIG REQUIRED COMPONENTS serialization)
    include_component_cmake_scripts(lux::cxx::serialization)

    add_executable(meta_projection_compatibility_test test/ProjectionCompatibility.cpp)
    target_include_directories(meta_projection_compatibility_test PRIVATE test)
    target_link_libraries(
        meta_projection_compatibility_test
        PRIVATE type_info lux::cxx::serialization lux::cxx::reflection_runtime
    )
    if(MSVC)
        target_compile_options(meta_projection_compatibility_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(meta_projection_compatibility_test PRIVATE -UNDEBUG)
    endif()
    # One real parser job, two unmodified production templates; no handwritten metadata substitute.
    lux_add_codegen_job(
        NAME meta_projection_compatibility
        GENERATOR "${LUX_META_GENERATOR}"
        MARKER luxref
        SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/test/ProjectionCompatibility.cpp"
        TARGET_FILES "${CMAKE_CURRENT_SOURCE_DIR}/test/ProjectionTypes.hpp"
        LOGICAL_PATHS ProjectionTypes.hpp
    )
    engine_type_static_info_template(static_info_template)
    lux_codegen_add_projection(
        JOB meta_projection_compatibility
        NAME engine_fields
        TEMPLATE "${static_info_template}"
        OUTPUT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/projection_test"
        OUTPUT_SUFFIX .type_static_info.hpp
    )
    lux_codegen_add_projection(
        JOB meta_projection_compatibility
        NAME serialization_fields
        TEMPLATE "${LUX_SERIALIZATION_TEMPLATE}"
        OUTPUT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/projection_test"
        OUTPUT_SUFFIX .serialize.hpp
    )
    lux_target_add_codegen(
        TARGET meta_projection_compatibility_test
        JOB meta_projection_compatibility
        DONT_ADD_TO_SOURCE
    )
    add_test(NAME meta.projection_compatibility COMMAND meta_projection_compatibility_test)

    add_executable(meta_runtime_object_test test/RuntimeObjectTest.cpp)

    target_link_libraries(meta_runtime_object_test PRIVATE lux::engine::core::meta)

    add_test(NAME meta.runtime_object COMMAND meta_runtime_object_test)

    add_library(meta_runtime_object_plugin SHARED test/RuntimeObjectPlugin.cpp)

    target_link_libraries(meta_runtime_object_plugin PRIVATE lux::engine::core::meta)

    add_executable(meta_runtime_object_plugin_test test/RuntimeObjectPluginTest.cpp)

    target_link_libraries(
        meta_runtime_object_plugin_test
        PRIVATE lux::engine::core::meta lux::engine::platform::dynamic_library
    )

    add_dependencies(meta_runtime_object_plugin_test meta_runtime_object_plugin)

    add_test(
        NAME meta.runtime_object_plugin
        COMMAND meta_runtime_object_plugin_test "$<TARGET_FILE:meta_runtime_object_plugin>"
    )
endif()
