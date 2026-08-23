# Runtime metadata uses the reflection vocabulary and generic containers, but
# never the libclang-backed generator library.  The generator is consumed only
# by engine_add_meta.cmake as a BUILD_TOOL.
find_package(
    lux-cxx
    CONFIG REQUIRED
    COMPONENTS algorithm compile_time container reflection_runtime
)

set(LUX_META_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Meta.cpp
)

add_component(
    COMPONENT_NAME  meta
    NAMESPACE       lux::engine::core
    OUTPUT_NAME     lux_engine_core_meta
    SOURCE_FILES    ${LUX_META_SRCS}
)

component_include_directories(
    meta
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME        include
    PRIVATE             pinclude
)

target_compile_definitions(
    meta
    PRIVATE
    LUX_CORE_LIBRARY
)

component_add_transitive_commands(
    meta
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time container reflection_runtime)"
)

component_add_cmake_scripts(
	meta
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_add_meta.cmake
)

install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/template/
    DESTINATION share/lux-engine-core/meta/template
)

target_link_libraries(
    meta
    PUBLIC
    lux::cxx::algorithm
    lux::cxx::compile_time
    lux::cxx::container
    lux::cxx::reflection_runtime
)

lux_classify_target(
    TARGET  meta
    LAYER   CORE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

# Pure-CPU, no device, ~0s. Gated like the other suites, but defaulting ON:
# the property it guards (a runtime-loaded module's types become visible
# WITHOUT wiping the ones already registered) fails silently — an unregistered
# component makes Scene::load skip it, so the file loads, the entity count is
# right, and every entity owns nothing. Cheap to run, expensive to miss.
set(ENABLE_META_TEST true CACHE BOOL "Enable meta reflection tests")
if(ENABLE_META_TEST)
    add_executable(reflection_drain_test test/reflection_drain_test.cpp)
    target_link_libraries(reflection_drain_test PRIVATE meta)
endif()
