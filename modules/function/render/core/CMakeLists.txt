find_package(lux-cxx CONFIG REQUIRED COMPONENTS container compile_time algorithm core)

add_component(
    STATIC
    COMPONENT_NAME render_core
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_core
    SOURCE_FILES src/Target.cpp src/RasterViewport.cpp
)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../../core/error/cmake/Values.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../resource/description/cmake/ValueTypes.cmake")

component_include_directories(
    render_core
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    INSTALL_TIME include
)

target_link_libraries(render_core PUBLIC lux::cxx::container lux::cxx::compile_time lux::cxx::algorithm lux::cxx::core
    lux::engine::core::error_values lux::engine::resource::description_types
)

component_add_transitive_commands(
    render_core
    "find_package(lux-cxx CONFIG REQUIRED COMPONENTS container compile_time algorithm core)"
    "find_package(lux-engine-core CONFIG REQUIRED COMPONENTS error_values)"
    "find_package(lux-engine-resource CONFIG REQUIRED COMPONENTS description_types)"
)

target_compile_features(render_core PUBLIC cxx_std_20)
