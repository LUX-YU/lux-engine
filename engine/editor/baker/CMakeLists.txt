## engine/editor/baker — pre-generates the editor's built-in .luxasset files.
##
## Build-time tool only. Outputs go under ${CMAKE_BINARY_DIR}/engine_content/
## and are loaded back at runtime by EditorBuiltins. Source tree has no
## .luxasset files; everything is regenerated on each fresh build.

add_executable(
    lux_content_baker
    ${CMAKE_CURRENT_SOURCE_DIR}/lux_content_baker.cpp
)

target_link_libraries(
    lux_content_baker
    PRIVATE
        editor                              # for EditorBuiltins.hpp UUID literals
        lux::engine::resource::asset
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
