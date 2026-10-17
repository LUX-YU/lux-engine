find_package(lux-cxx CONFIG REQUIRED COMPONENTS core binary memory)

# Reuse the existing neutral stage declaration, without linking the resource/asset implementation.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../resource/description/cmake/ShaderMetadata.cmake")

add_component(
    STATIC
    COMPONENT_NAME render_data
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_data
    SOURCE_FILES src/Schema.cpp src/Encoding.cpp src/Payload.cpp
)

component_include_directories(
    render_data
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    INSTALL_TIME include
)

component_add_internal_dependencies(render_data lux::engine::render::render_core)

target_link_libraries(render_data PUBLIC lux::cxx::core lux::cxx::binary lux::cxx::memory)

target_link_libraries(render_data PUBLIC $<BUILD_INTERFACE:lux::engine::resource::shader_metadata>)

install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../../resource/description/include/lux/engine/description/ShaderInfo.hpp"
    DESTINATION include/lux/engine/description
)

install(
    FILES "${LUX_GENERATE_HEADER_DIR}/lux/engine/resource/visibility.h"
    DESTINATION include/lux/engine/resource
)

component_add_transitive_commands(
    render_data
    "find_package(lux-cxx CONFIG REQUIRED COMPONENTS core binary memory)"
)

target_compile_features(render_data PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
