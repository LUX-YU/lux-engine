find_package(lux-cxx CONFIG REQUIRED COMPONENTS concurrent)

add_component(
    STATIC
    COMPONENT_NAME render_transport
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_transport
    SOURCE_FILES src/Error.cpp src/Routes.cpp src/Replies.cpp src/Packet.cpp src/Transport.cpp
)

component_include_directories(
    render_transport
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    INSTALL_TIME include
)

target_link_libraries(render_transport PUBLIC lux::engine::render::render_core lux::cxx::concurrent)

target_compile_features(render_transport PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
