include_guard(GLOBAL)

# Header-only neutral contract boundary; no Math, Script, Asset or Engine SDK.
add_interface_component(
    COMPONENT_NAME description_types
    NAMESPACE lux::engine::resource
)

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_RESOURCE_LIBRARY
    PUBLIC_MACRO_NAME LUX_RESOURCE_PUBLIC
    GENERATE_FILE_PATH lux/engine/resource/visibility.h
)

target_include_directories(
    description_types INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include>"
    "$<BUILD_INTERFACE:${LUX_GENERATE_HEADER_DIR}>"
    "$<INSTALL_INTERFACE:include>"
)

install(
    FILES "${CMAKE_CURRENT_LIST_DIR}/../include/lux/engine/description/ShaderInfo.hpp"
        "${CMAKE_CURRENT_LIST_DIR}/../include/lux/engine/description/ShaderBlock.hpp"
        "${CMAKE_CURRENT_LIST_DIR}/../include/lux/engine/description/DataLayout.hpp"
        "${CMAKE_CURRENT_LIST_DIR}/../include/lux/engine/description/PassContract.hpp"
        "${CMAKE_CURRENT_LIST_DIR}/../include/lux/engine/description/Image.hpp"
        "${CMAKE_CURRENT_LIST_DIR}/../include/lux/engine/description/TextureInfo.hpp"
        "${CMAKE_CURRENT_LIST_DIR}/../include/lux/engine/description/MaterialEnums.hpp"
        "${CMAKE_CURRENT_LIST_DIR}/../include/lux/engine/description/MaterialLayout.hpp"
    DESTINATION include/lux/engine/description
)

install(
    FILES "${LUX_GENERATE_HEADER_DIR}/lux/engine/resource/visibility.h"
    DESTINATION include/lux/engine/resource
)
