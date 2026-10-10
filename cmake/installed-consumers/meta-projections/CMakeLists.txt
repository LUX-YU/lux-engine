cmake_minimum_required(VERSION 3.28)

project(InstalledMetaProjections LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()

find_package(lux-engine-core CONFIG REQUIRED COMPONENTS meta)
find_package(lux-cxx CONFIG REQUIRED COMPONENTS serialization reflection_runtime)
include_component_cmake_scripts(lux::engine::core::meta)
include_component_cmake_scripts(lux::cxx::serialization)

set(fixture "${CMAKE_CURRENT_SOURCE_DIR}/../../../modules/core/meta/test")
add_executable(meta_projections "${fixture}/ProjectionCompatibility.cpp")
target_include_directories(meta_projections PRIVATE "${fixture}")
target_link_libraries(
    meta_projections
    PRIVATE lux::engine::core::type_info lux::cxx::serialization lux::cxx::reflection_runtime
)
if(MSVC)
    target_compile_options(meta_projections PRIVATE /UNDEBUG /utf-8 /permissive- /GR-)
else()
    target_compile_options(meta_projections PRIVATE -UNDEBUG -fno-rtti)
endif()

lux_add_codegen_job(
    NAME installed_meta_projections
    GENERATOR "${LUX_META_GENERATOR}"
    MARKER luxref
    SOURCE_FILE "${fixture}/ProjectionCompatibility.cpp"
    TARGET_FILES "${fixture}/ProjectionTypes.hpp"
    LOGICAL_PATHS ProjectionTypes.hpp
)
engine_type_static_info_template(static_info_template)
lux_codegen_add_projection(
    JOB installed_meta_projections
    NAME engine_fields
    TEMPLATE "${static_info_template}"
    OUTPUT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/generated"
    OUTPUT_SUFFIX .type_static_info.hpp
)
lux_codegen_add_projection(
    JOB installed_meta_projections
    NAME serialization_fields
    TEMPLATE "${LUX_SERIALIZATION_TEMPLATE}"
    OUTPUT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/generated"
    OUTPUT_SUFFIX .serialize.hpp
)
lux_target_add_codegen(
    TARGET meta_projections
    JOB installed_meta_projections
    DONT_ADD_TO_SOURCE
)
add_test(NAME installed.meta.projections COMMAND meta_projections)
