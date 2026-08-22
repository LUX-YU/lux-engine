if(ENABLE_SUPERBUILD)
    set(STB_SOURCE_DIR ${THIRD_PARTY_SOURCE_DIR}/stb/stb-master)
else()
    find_path(
        STB_SOURCE_DIR
        "stb_image.h"
        REQUIRED
        HINTS
            "/usr/include/stb"
            "/usr/local/include/stb"
            "/usr/include"
            "/usr/local/include"
    )
endif()

set(
    BC7ENC_SOURCE_DIR
    ${CMAKE_SOURCE_DIR}/modules/resource/asset/third_party/bc7enc_rdo
)

add_component(
    STATIC
    COMPONENT_NAME toolchain_texture_importer
    NAMESPACE      lux::engine::toolchain
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TextureImporter.cpp
        ${BC7ENC_SOURCE_DIR}/rgbcx.cpp
        ${BC7ENC_SOURCE_DIR}/bc7enc.cpp
)

component_include_directories(
    toolchain_texture_importer
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME
        include
    PRIVATE
        ${STB_SOURCE_DIR}
        ${BC7ENC_SOURCE_DIR}
)

target_compile_definitions(
    toolchain_texture_importer
    PRIVATE
        RGBCX_IMPLEMENTATION
)

target_link_libraries(
    toolchain_texture_importer
    PUBLIC
        lux::engine::resource::asset
)

set_target_properties(
    toolchain_texture_importer
    PROPERTIES POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
    set_source_files_properties(
        ${BC7ENC_SOURCE_DIR}/rgbcx.cpp
        ${BC7ENC_SOURCE_DIR}/bc7enc.cpp
        PROPERTIES COMPILE_OPTIONS "/W0"
    )
else()
    set_source_files_properties(
        ${BC7ENC_SOURCE_DIR}/rgbcx.cpp
        ${BC7ENC_SOURCE_DIR}/bc7enc.cpp
        PROPERTIES COMPILE_OPTIONS "-w"
    )
endif()

lux_classify_target(
    TARGET  toolchain_texture_importer
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)
