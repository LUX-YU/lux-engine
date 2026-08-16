find_package(Vulkan REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME   LUX_PLATFORM_GAPI_LIBRARY
    PUBLIC_MACRO_NAME   LUX_PLATFORM_GAPI_PUBLIC
    GENERATE_FILE_PATH  lux/engine/gapi/visibility.h
)

# Component: gapi
# -----------------
add_interface_component(
    COMPONENT_NAME gapi
    NAMESPACE      lux::engine::platform
)

component_include_directories(
    gapi
    BUILD_TIME_EXPORT  
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    gapi
    INTERFACE
    VULKAN_SUPPORT
    VULKAN_DEBUG_REPORT
    # PRIVATE
    # LUX_PLATFORM_GAPI_LIBRARY
)

target_link_libraries(
    gapi
    INTERFACE
    Vulkan::Vulkan
    lux::cxx::compile_time
)

component_add_transitive_commands(
    gapi
    "find_package(Vulkan    REQUIRED)"
)

lux_classify_target(
    TARGET  gapi
    LAYER   PLATFORM
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)
