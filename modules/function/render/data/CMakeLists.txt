find_package(lux-cxx CONFIG REQUIRED COMPONENTS core)

# Reuse the existing neutral stage declaration, without linking the resource/asset implementation.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../resource/description/cmake/ValueTypes.cmake")

add_interface_component(
    COMPONENT_NAME render_data
    NAMESPACE lux::engine::render
)

component_include_directories(
    render_data
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    INSTALL_TIME include
)

component_add_internal_dependencies(render_data lux::engine::render::render_core)

target_link_libraries(render_data INTERFACE lux::cxx::core)

target_link_libraries(render_data INTERFACE lux::engine::resource::description_types)

component_add_transitive_commands(
    render_data
    "find_package(lux-cxx CONFIG REQUIRED COMPONENTS core)"
    "find_package(lux-engine-resource CONFIG REQUIRED COMPONENTS description_types)"
)

target_compile_features(render_data INTERFACE cxx_std_20)
