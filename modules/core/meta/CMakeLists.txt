set(LUX_META_SRCS
    src/LuxObject.cpp
)

find_package(lux-cxx REQUIRED COMPONENTS dynamic_reflection)

add_component(
    COMPONENT_NAME  meta
    NAMESPACE       lux::engine::core
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

target_link_libraries(
    meta
    PUBLIC
    lux::cxx::dynamic_reflection
)