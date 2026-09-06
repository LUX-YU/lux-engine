## core::serialization — domain-neutral binary serialization primitives.
##
## Provides exact little-endian binary primitives plus compile-time generic
## serialization. It has no runtime-reflection fallback.
##
## This target deliberately has no reflection, Eigen, ECS, scene, asset,
## runtime, render or editor knowledge. Domain formats build on this foundation.

find_package(stduuid CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS binary compile_time core)
find_package(Eigen3 CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_CORE_SERIALIZATION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_CORE_SERIALIZATION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_CORE_SERIALIZATION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/serialization/visibility.h
)

set(LUX_SERIALIZE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Serialization.cpp
)

add_component(
    COMPONENT_NAME  serialization
    NAMESPACE       lux::engine::core
    OUTPUT_NAME     lux_engine_core_serialization
    SOURCE_FILES    ${LUX_SERIALIZE_SRCS}
)

component_include_directories(
    serialization
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME        include
)

target_compile_definitions(
    serialization
    PRIVATE
    LUX_CORE_SERIALIZATION_LIBRARY
)

target_link_libraries(
    serialization
    PUBLIC
    lux::cxx::binary
    lux::cxx::compile_time
    lux::cxx::core
    Eigen3::Eigen
    stduuid
)

component_add_internal_dependencies(serialization type_info)

component_add_transitive_commands(
    serialization
    "find_package(lux-cxx REQUIRED COMPONENTS binary compile_time core)"
    "find_package(stduuid CONFIG REQUIRED)"
    "find_package(Eigen3 CONFIG REQUIRED)"
)

lux_classify_target(
    TARGET  serialization
    LAYER   CORE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)
