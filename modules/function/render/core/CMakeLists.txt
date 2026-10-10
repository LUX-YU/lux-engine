find_package(lux-cxx CONFIG REQUIRED COMPONENTS core container algorithm compile_time)

add_component(
    STATIC
    COMPONENT_NAME render_core
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_core
    SOURCE_FILES src/Descriptors.cpp
)

# Only the generic error value/descriptor headers are needed. Linking the error
# registry would introduce a runtime service and synchronization into Core.
get_filename_component(LUX_CORE_ERROR_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/error/include" ABSOLUTE)

component_include_directories(
    render_core
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    BUILD_TIME_SHARED "${LUX_CORE_ERROR_INCLUDE}"
    INSTALL_TIME include
)

target_link_libraries(
    render_core
    PUBLIC lux::cxx::core lux::cxx::container lux::cxx::algorithm lux::cxx::compile_time
)

target_compile_features(render_core PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
