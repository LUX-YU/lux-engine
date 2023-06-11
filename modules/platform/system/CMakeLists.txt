add_component(
    COMPONENT_NAME                  system
    NAMESPACE                       lux::engine::platform
    SOURCE_FILES                    src/memory_pool.cpp
                                    src/thread_pool.cpp
                                    src/Introspection.cpp
	BUILD_TIME_EXPORT_INCLUDE_DIRS  ${CMAKE_CURRENT_SOURCE_DIR}/include
                                    ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME_INCLUDE_PREFIX     include
    PRIVATE_DEFINITIONS             LUX_PLATFORM_LIBRARY
)
