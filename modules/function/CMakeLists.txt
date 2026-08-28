message("-------- Function Layer --------")

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_FUNCTION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_FUNCTION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_FUNCTION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/function/visibility.h
)

if(NOT TARGET script_core)
    add_subdirectory(script/core)
endif()
add_subdirectory(flowforge)

if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    # EntityScene cooks materialize built-in 3D component schemas. Their
    # schema-only ECS targets use the backend-neutral render client vocabulary
    # and animation value types, but never render_graph/vulkan/features, UI,
    # Input or Lua. Keep this list explicit so an offline-tool configure cannot
    # discover Vulkan, GLFW, ImGui or a Runtime script backend by directory
    # enumeration.
    add_subdirectory(animation)
    add_subdirectory(render/client)
    add_subdirectory(render/standard_content)
    # Spatial3D EntityScene cooking emits the engine-owned LXNR payload through
    # navigation_detour3d's codec.  This lower-layer dependency is Recast-only
    # and must exist in the Toolchain profile without pulling Render/Vulkan.
    add_subdirectory(navigation)
    if(LUX_ENABLE_FLOWFORGE_MLIR)
        add_subdirectory(script)
    endif()
else()
    add_subdirectory(animation)
    add_subdirectory(input)
    add_subdirectory(navigation)
    add_subdirectory(render)
    add_subdirectory(script)
    add_subdirectory(ui)
endif()

if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    set(LUX_FUNCTION_COMPONENTS
        animation
        flowforge
        navigation
        navigation_detour3d
        render_client
        render_standard_content
        script_artifact
        script_core
    )
    if(LUX_ENABLE_FLOWFORGE_MLIR)
        if(ENABLE_FLOWFORGE_TEST)
            list(APPEND LUX_FUNCTION_COMPONENTS script_native)
        endif()
    endif()
else()
    set(LUX_FUNCTION_COMPONENTS
        flowforge
        render_client
        render_standard_content
        render_graph
        render_vulkan
        navigation
        navigation_detour3d
        input
        animation
        script_artifact
        script_core
        script_native
    )
    if(TARGET script_lua)
        list(APPEND LUX_FUNCTION_COMPONENTS script_lua)
    endif()
    if(TARGET render_features)
        list(APPEND LUX_FUNCTION_COMPONENTS render_features)
    endif()
    list(APPEND LUX_FUNCTION_COMPONENTS ui)
endif()

if(LUX_FUNCTION_COMPONENTS)
    install_components(
        PROJECT_NAME lux-engine-function
        VERSION      0.0.1
        NAMESPACE    lux::engine::function
        COMPONENTS   ${LUX_FUNCTION_COMPONENTS}
    )
endif()
