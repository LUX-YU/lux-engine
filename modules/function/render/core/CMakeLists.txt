find_package(lux-cxx CONFIG REQUIRED COMPONENTS container compile_time algorithm)

add_component(
    STATIC
    COMPONENT_NAME render_core
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_core
    SOURCE_FILES src/Target.cpp
)

get_filename_component(LUX_RENDER_ERROR_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/error/include" ABSOLUTE)
get_filename_component(
    LUX_RENDER_DESCRIPTION_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../../resource/description/include" ABSOLUTE
)

component_include_directories(
    render_core
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    BUILD_TIME_SHARED "${LUX_RENDER_ERROR_INCLUDE}" "${LUX_RENDER_DESCRIPTION_INCLUDE}"
    INSTALL_TIME include
)

target_link_libraries(render_core PUBLIC lux::cxx::container lux::cxx::compile_time lux::cxx::algorithm)

component_add_transitive_commands(
    render_core
    "find_package(lux-cxx CONFIG REQUIRED COMPONENTS container compile_time algorithm)"
)

target_compile_features(render_core PUBLIC cxx_std_20)

# Reuse these exact canonical value headers. No copied implementation, error service,
# shader/asset library or script dependency is needed by this small header closure.
install(
    FILES "${LUX_RENDER_ERROR_INCLUDE}/lux/engine/error/Error.hpp"
        "${LUX_RENDER_ERROR_INCLUDE}/lux/engine/error/ErrorDescriptor.hpp"
    DESTINATION include/lux/engine/error
)

install(
    FILES "${LUX_RENDER_DESCRIPTION_INCLUDE}/lux/engine/description/Image.hpp"
    DESTINATION include/lux/engine/description
)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
