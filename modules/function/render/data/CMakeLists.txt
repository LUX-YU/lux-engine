add_library(render_data STATIC src/Schema.cpp)

add_library(lux::engine::render::render_data ALIAS render_data)

target_include_directories(render_data PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:include>"
)

target_link_libraries(render_data PUBLIC render_core shader_metadata)

target_compile_features(render_data PUBLIC cxx_std_20)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RenderDataCodegen.cmake")

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
