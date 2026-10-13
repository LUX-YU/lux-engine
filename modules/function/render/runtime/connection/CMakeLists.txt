find_package(lux-cxx CONFIG REQUIRED COMPONENTS concurrent container)

# Deliberately not add_component: this implementation is not an installed SDK component.
add_library(render_connection STATIC src/Connection.cpp)

target_include_directories(render_connection PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/sinclude")

target_link_libraries(render_connection PUBLIC render_protocol PRIVATE lux::cxx::concurrent lux::cxx::container)

target_compile_features(render_connection PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
