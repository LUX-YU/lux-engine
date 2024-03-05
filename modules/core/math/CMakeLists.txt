find_package (Eigen3 REQUIRED)

set(MATH_SRCS
    src/eigen_extend.cpp
)

add_component(
    COMPONENT_NAME math
    NAMESPACE      lux::engine::core
    SOURCE_FILES   ${MATH_SRCS}
)

component_include_directories(
    math
    BUILD_TIME_EXPORT  
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    math
    PRIVATE
    LUX_CORE_LIBRARY
)

target_link_libraries(
    math
    PUBLIC
    Eigen3::Eigen
)

component_add_transitive_commands(
    math
    "find_package (Eigen3 REQUIRED)"
)
