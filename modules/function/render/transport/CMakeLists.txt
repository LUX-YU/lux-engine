find_package(lux-cxx CONFIG REQUIRED COMPONENTS concurrent container)

add_component(
    STATIC
    COMPONENT_NAME render_transport
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_transport
    SOURCE_FILES src/Routes.cpp src/Replies.cpp src/Transport.cpp
)

component_include_directories(
    render_transport
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    INSTALL_TIME include
)

target_include_directories(render_transport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pinclude")

component_add_internal_dependencies(render_transport lux::engine::render::render_data)

target_link_libraries(render_transport PUBLIC lux::cxx::container PRIVATE lux::cxx::concurrent)

component_add_transitive_commands(
    render_transport
    "find_package(lux-cxx CONFIG REQUIRED COMPONENTS concurrent container)"
)

target_compile_features(render_transport PUBLIC cxx_std_20)

get_target_property(transport_links render_transport INTERFACE_LINK_LIBRARIES)

set(expected_links lux::engine::render::render_data lux::cxx::container "$<LINK_ONLY:lux::cxx::concurrent>")

if(NOT transport_links STREQUAL expected_links)
    message(FATAL_ERROR "Unexpected Transport dependency: ${transport_links}")
endif()

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
