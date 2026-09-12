add_executable(lux_scene_seed src/SceneSeed.cpp)
target_link_libraries(lux_scene_seed PRIVATE lux::engine::toolchain::toolchain_material_cooker
    lux::engine::resource::asset)

if(WIN32)
    add_custom_command(TARGET lux_scene_seed POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:lux_scene_seed>
            $<TARGET_FILE_DIR:lux_scene_seed>
        COMMAND_EXPAND_LISTS VERBATIM)
endif()
