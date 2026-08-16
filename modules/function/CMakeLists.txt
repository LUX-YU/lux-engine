message("-------- Function Layer --------")

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_FUNCTION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_FUNCTION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_FUNCTION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/function/visibility.h
)

# get sublist
subdirectory_list(dir_list)

if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    # EntityScene cooks materialize built-in 3D component schemas. Their
    # schema-only ECS targets use the backend-neutral render client vocabulary
    # and animation value types, but never render_graph/vulkan/features, UI,
    # Input or Lua. Keep this list explicit so an offline-tool configure cannot
    # discover Vulkan, GLFW, ImGui or a Runtime script backend by directory
    # enumeration.
    set(dir_list animation render/client)
    # Spatial3D EntityScene cooking emits the engine-owned LXNR payload through
    # navigation_detour3d's codec.  This lower-layer dependency is Recast-only
    # and must exist in the Toolchain profile without pulling Render/Vulkan.
    list(APPEND dir_list navigation)
    if(LUX_ENABLE_FLOWFORGE_MLIR)
        list(APPEND dir_list script)
    endif()
endif()

foreach(subdir ${dir_list})
    # ui is the imgui-over-GLFW overlay stack (find_package(imgui COMPONENTS
    # glfw3)) — desktop-only until the ImGui_ImplAndroid backend lands
    # (mobile investigation §3 item 3-6). Staged, not cut.
    if(ANDROID AND subdir STREQUAL "ui")
        continue()
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/CMakeLists.txt)
        add_subdirectory(${subdir})
    endif()
endforeach()

if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    set(LUX_FUNCTION_COMPONENTS
        animation
        navigation
        navigation_detour3d
        render_client
    )
    if(LUX_ENABLE_FLOWFORGE_MLIR)
        list(APPEND LUX_FUNCTION_COMPONENTS script_core)
        if(ENABLE_FLOWFORGE_TEST)
            list(APPEND LUX_FUNCTION_COMPONENTS script_native)
        endif()
    endif()
else()
    set(LUX_FUNCTION_COMPONENTS
        render_client
        render_graph
        render_vulkan
        render_features
        navigation
        navigation_detour3d
        input
        animation
        script_core
        script_lua
        script_native
    )
    if(NOT ANDROID)
        list(APPEND LUX_FUNCTION_COMPONENTS ui ui_vulkan)
    endif()
endif()

if(LUX_FUNCTION_COMPONENTS)
    install_components(
        PROJECT_NAME lux-engine-function
        VERSION      0.0.1
        NAMESPACE    lux::engine::function
        COMPONENTS   ${LUX_FUNCTION_COMPONENTS}
    )
endif()
