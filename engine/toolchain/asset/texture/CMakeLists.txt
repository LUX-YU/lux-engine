find_path(LUX_STB_INCLUDE_DIR NAMES stb_image.h REQUIRED)

# These are the preserved MIT/public-domain encoder sources. Only this
# Toolchain BUILD_TOOL compiles them; they never enter a Runtime/Player link
# or interface closure.
set(LUX_BC_ENCODER_SOURCE_DIR
    "${CMAKE_SOURCE_DIR}/legacy/modules/resource/asset-runtime/third_party/bc7enc_rdo"
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_TEXTURE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_TEXTURE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_TOOLCHAIN_TEXTURE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/toolchain/asset/texture/visibility.h
)

add_component(
    STATIC
    COMPONENT_NAME toolchain_texture_cooker
    NAMESPACE      lux::engine::toolchain
    OUTPUT_NAME    lux_engine_toolchain_texture_cooker
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TextureCooker.cpp
        ${LUX_BC_ENCODER_SOURCE_DIR}/rgbcx.cpp
        ${LUX_BC_ENCODER_SOURCE_DIR}/bc7enc.cpp
)

component_include_directories(
    toolchain_texture_cooker
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE
        ${LUX_STB_INCLUDE_DIR}
        ${LUX_BC_ENCODER_SOURCE_DIR}
)

target_compile_definitions(
    toolchain_texture_cooker
    PUBLIC
        LUX_ENGINE_TOOLCHAIN_TEXTURE_DLL_DISABLE
    PRIVATE LUX_ENGINE_TOOLCHAIN_TEXTURE_LIBRARY
        STBI_ONLY_BMP
        STBI_ONLY_GIF
        STBI_ONLY_HDR
        STBI_ONLY_JPEG
        STBI_ONLY_PNG
        STBI_ONLY_PNM
        STBI_ONLY_PSD
        STBI_ONLY_TGA
)

target_link_libraries(
    toolchain_texture_cooker
    PUBLIC lux::engine::resource::asset
)

component_add_transitive_commands(
    toolchain_texture_cooker
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset description)"
)

if(MSVC)
    set_source_files_properties(
        ${LUX_BC_ENCODER_SOURCE_DIR}/rgbcx.cpp
        ${LUX_BC_ENCODER_SOURCE_DIR}/bc7enc.cpp
        PROPERTIES COMPILE_OPTIONS "/W0"
    )
else()
    set_source_files_properties(
        ${LUX_BC_ENCODER_SOURCE_DIR}/rgbcx.cpp
        ${LUX_BC_ENCODER_SOURCE_DIR}/bc7enc.cpp
        PROPERTIES COMPILE_OPTIONS "-w"
    )
endif()

lux_classify_target(
    TARGET  toolchain_texture_cooker
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)

if(BUILD_TESTING)
    add_executable(
        toolchain_texture_cooker_test
        ${CMAKE_CURRENT_SOURCE_DIR}/test/TextureCookerTest.cpp
    )
    target_link_libraries(
        toolchain_texture_cooker_test
        PRIVATE lux::engine::toolchain::toolchain_texture_cooker
    )
    if(MSVC)
        target_compile_options(toolchain_texture_cooker_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(toolchain_texture_cooker_test PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(
        TARGET  toolchain_texture_cooker_test
        LAYER   TOOLCHAIN
        PRODUCT TEST
        ROLE    DOMAIN
    )
    add_test(NAME toolchain_texture_cooker_test COMMAND toolchain_texture_cooker_test)
endif()
