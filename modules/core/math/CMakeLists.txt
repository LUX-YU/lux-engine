find_package (Eigen3 REQUIRED)

set(MATH_SRCS
    src/eigen_extend.cpp
)

add_component(
    COMPONENT_NAME                  math
    NAMESPACE                       lux::engine::core
    SOURCE_FILES                    ${MATH_SRCS}
	BUILD_TIME_EXPORT_INCLUDE_DIRS  ${CMAKE_CURRENT_SOURCE_DIR}/include
                                    ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME_INCLUDE_PREFIX     include
    PUBLIC_LIBRARIES                Eigen3::Eigen
    PRIVATE_DEFINITIONS             LUX_CORE_LIBRARY
    TRANSITIVE_PACKAGES_COMMANDS    "find_package (Eigen3 REQUIRED)"
)
