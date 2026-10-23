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
add_subdirectory(graph)
add_subdirectory(flowforge)
if(
    (LUX_PROFILE_HAS_EDITOR OR LUX_PROFILE_HAS_TOOLCHAIN OR LUX_BUILD_PACKED_RENDER_CONTENT) AND
    NOT TARGET material_graph
)
    add_subdirectory(material)
endif()

if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    # Offline tools consume authoring and resource descriptions. Render is a runtime library;
    # it is not pulled in through obsolete client/content component names.
    add_subdirectory(animation)
    # Spatial3D EntityScene cooking emits the engine-owned LXNR payload through
    # navigation_detour3d's codec.  This lower-layer dependency is Recast-only
    # and must exist in the Toolchain profile without pulling Render/Vulkan.
    add_subdirectory(navigation)
    add_subdirectory(script)
else()
    add_subdirectory(animation)
    add_subdirectory(input)
    add_subdirectory(navigation)
    add_subdirectory(render)
    add_subdirectory(script)
    add_subdirectory(ui)
endif()

if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    set(
        LUX_FUNCTION_COMPONENTS
        animation
        flowforge
        graph
        navigation
        navigation_detour3d
        script_artifact
        script_core
    )

    list(APPEND LUX_FUNCTION_COMPONENTS script_native)
else()
    set(
        LUX_FUNCTION_COMPONENTS
        flowforge
        graph
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
    list(APPEND LUX_FUNCTION_COMPONENTS ui ui_input)
endif()

if(TARGET material_graph)
    list(APPEND LUX_FUNCTION_COMPONENTS material_graph)
endif()

if(BUILD_TESTING AND TARGET material_graph)
    add_executable(graph_domain_edit_test graph/test/domain_edit.cpp)

    target_link_libraries(
        graph_domain_edit_test
        PRIVATE lux::engine::function::material_graph lux::engine::function::flowforge
    )

    add_test(NAME graph.material_edit COMMAND graph_domain_edit_test material)

    add_test(NAME graph.flow_edit COMMAND graph_domain_edit_test flow)
endif()


if(LUX_FUNCTION_COMPONENTS)
    lux_engine_install_components(
        PROJECT_NAME lux-engine-function
        VERSION      0.0.1
        NAMESPACE    lux::engine::function
        COMPONENTS   ${LUX_FUNCTION_COMPONENTS}
    )
endif()
