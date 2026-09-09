# Optional CMAKE_PROJECT_INCLUDE for a dedicated RelWithDebInfo diagnostic tree.
# This file is not installed and does not alter the normal SDK's compiler flags.
function(lux_editor_address_checks)
    if(NOT LUX_EDITOR_DIAGNOSTICS OR NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR NOT MSVC)
        message(FATAL_ERROR "Editor address checks require the isolated MSVC RelWithDebInfo diagnostics tree")
    endif()
    foreach(target IN ITEMS meta ui ui_imgui_backend ui_vulkan_backend editor_ui editor_scene_session
            editor_rendering render_client scene_render scene_composition editor_application
            editor_application_candidate lux_editor lux_editor_er1 editor_application_lifecycle_test
            editor_scene_gpu_test editor_foreign_renderer_test)
        if(NOT TARGET ${target})
            message(FATAL_ERROR "Address-check target missing: ${target}")
        endif()
        target_compile_options(${target} PRIVATE /fsanitize=address)
        target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    endforeach()
endfunction()
cmake_language(DEFER CALL lux_editor_address_checks)
