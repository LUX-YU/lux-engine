## Build-tool leaf that materialises the engine's cooked built-in content.
##
## Build-time tool only. Outputs go under ${CMAKE_BINARY_DIR}/engine_content/
## and are loaded back at runtime by EditorBuiltins. Source tree has no
## .luxasset files; everything is regenerated on each fresh build.

add_executable(
    lux_content_baker
    ${CMAKE_CURRENT_SOURCE_DIR}/lux_content_baker.cpp
)

lux_classify_target(
    TARGET lux_content_baker
    LAYER TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE COMPOSITION
)

target_link_libraries(
    lux_content_baker
    PRIVATE
        lux::engine::resource::asset_codecs
        lux::engine::toolchain::toolchain_builtin_content
        lux::engine::toolchain::toolchain_material_compiler
)

if(WIN32)
    add_custom_command(TARGET lux_content_baker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:lux_content_baker>
                $<TARGET_FILE_DIR:lux_content_baker>
        COMMAND_EXPAND_LISTS
        COMMENT "Copy runtime DLLs for lux_content_baker"
    )
endif()
