set(LUX_META_SRCS
    src/LuxObject.cpp
)

add_component(
    COMPONENT_NAME                  meta
    NAMESPACE                       lux::engine::core
    SOURCE_FILES                    ${LUX_META_SRCS}
	BUILD_TIME_EXPORT_INCLUDE_DIRS  ${CMAKE_CURRENT_SOURCE_DIR}/include
                                    ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME_INCLUDE_PREFIX     include
    PRIVATE_INCLUDE_DIRS            pinclude
    PRIVATE_DEFINITIONS             LUX_CORE_LIBRARY
)
