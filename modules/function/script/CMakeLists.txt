add_subdirectory(core)
if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    # The AOT compiler consumes only the backend contract.  Native loading is
    # needed by its optional end-to-end tests; Lua is a Runtime backend.
    if(ENABLE_FLOWFORGE_TEST)
        add_subdirectory(native)
    endif()
else()
    if(LUX_SCRIPT_HAS_LUA)
        add_subdirectory(lua)
    endif()
    add_subdirectory(native)
endif()
