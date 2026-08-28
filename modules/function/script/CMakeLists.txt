if(NOT TARGET script_core)
    add_subdirectory(core)
endif()
add_subdirectory(artifact)
if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    # Native loading verifies that cooked FlowForge payloads are executable;
    # Lua remains a Runtime backend.
    add_subdirectory(native)
else()
    if(LUX_SCRIPT_HAS_LUA)
        add_subdirectory(lua)
    endif()
    add_subdirectory(native)
endif()
