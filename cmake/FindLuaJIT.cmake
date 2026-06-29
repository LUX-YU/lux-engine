# FindLuaJIT.cmake
# Finds LuaJIT library and headers
#
# This module defines:
#  LUAJIT_FOUND - True if LuaJIT is found
#  LUAJIT_INCLUDE_DIRS - Include directories for LuaJIT
#  LUAJIT_LIBRARIES - LuaJIT libraries
#  LUAJIT_VERSION - Version of LuaJIT found
#  LuaJIT::LuaJIT - Imported target for LuaJIT

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_LUAJIT QUIET luajit)
endif()

# Find the header file
find_path(LUAJIT_INCLUDE_DIR
    NAMES lua.h
    HINTS
        ${PC_LUAJIT_INCLUDEDIR}
        ${PC_LUAJIT_INCLUDE_DIRS}
    PATHS
        # Ubuntu/Debian paths
        /usr/include/luajit-2.1
        /usr/include/luajit-2.0
        /usr/local/include/luajit-2.1
        /usr/local/include/luajit-2.0
        # macOS Homebrew paths
        /opt/homebrew/include/luajit-2.1
        /usr/local/include/luajit-2.1
        # Windows vcpkg paths
        ${CMAKE_PREFIX_PATH}/include/luajit
        # Generic paths
        /usr/include
        /usr/local/include
        /opt/local/include
    PATH_SUFFIXES
        luajit-2.1
        luajit-2.0
        luajit
        lua
)

# Find the library
find_library(LUAJIT_LIBRARY
    NAMES lua51 luajit-5.1 luajit libluajit
    HINTS
        ${PC_LUAJIT_LIBDIR}
        ${PC_LUAJIT_LIBRARY_DIRS}
    PATHS
        /usr/lib
        /usr/local/lib
        /opt/homebrew/lib
        /opt/local/lib
        ${CMAKE_PREFIX_PATH}/lib
        ${CMAKE_PREFIX_PATH}/bin
    PATH_SUFFIXES
        x86_64-linux-gnu
        i386-linux-gnu
        aarch64-linux-gnu
)

# Extract version information
if(LUAJIT_INCLUDE_DIR)
    if(EXISTS "${LUAJIT_INCLUDE_DIR}/luaconf.h")
        file(READ "${LUAJIT_INCLUDE_DIR}/luaconf.h" LUAJIT_H_CONTENT)
        string(REGEX MATCH "#define[ \t]+LUA_VERSION_NUM[ \t]+([0-9]+)" _lua_version_match "${LUAJIT_H_CONTENT}")
        if(_lua_version_match)
            set(LUAJIT_VERSION_NUM ${CMAKE_MATCH_1})
            math(EXPR LUAJIT_VERSION_MAJOR "${LUAJIT_VERSION_NUM} / 10000")
            math(EXPR LUAJIT_VERSION_MINOR "(${LUAJIT_VERSION_NUM} % 10000) / 100")
            math(EXPR LUAJIT_VERSION_PATCH "${LUAJIT_VERSION_NUM} % 100")
            set(LUAJIT_VERSION "${LUAJIT_VERSION_MAJOR}.${LUAJIT_VERSION_MINOR}.${LUAJIT_VERSION_PATCH}")
        endif()
    endif()
    
    # Try to get LuaJIT specific version
    if(EXISTS "${LUAJIT_INCLUDE_DIR}/luajit.h")
        file(READ "${LUAJIT_INCLUDE_DIR}/luajit.h" LUAJIT_H_CONTENT)
        string(REGEX MATCH "#define[ \t]+LUAJIT_VERSION[ \t]+\"LuaJIT[ \t]+([0-9]+\\.[0-9]+\\.[0-9]+)" _luajit_version_match "${LUAJIT_H_CONTENT}")
        if(_luajit_version_match)
            set(LUAJIT_VERSION ${CMAKE_MATCH_1})
        endif()
    endif()
endif()

include(FindPackageHandleStandardArgs)  
find_package_handle_standard_args(LuaJIT
    REQUIRED_VARS LUAJIT_LIBRARY LUAJIT_INCLUDE_DIR
    VERSION_VAR LUAJIT_VERSION
)

if(LUAJIT_FOUND)
    set(LUAJIT_LIBRARIES ${LUAJIT_LIBRARY})
    set(LUAJIT_INCLUDE_DIRS ${LUAJIT_INCLUDE_DIR})
    
    # Create imported target
    if(NOT TARGET LuaJIT::LuaJIT)
        add_library(LuaJIT::LuaJIT UNKNOWN IMPORTED)
        set_target_properties(LuaJIT::LuaJIT PROPERTIES
            IMPORTED_LOCATION "${LUAJIT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LUAJIT_INCLUDE_DIR}"
        )
        
        # Link required system libraries on different platforms
        if(UNIX AND NOT APPLE)
            set_property(TARGET LuaJIT::LuaJIT PROPERTY INTERFACE_LINK_LIBRARIES "dl;m")
        elseif(APPLE)
            set_property(TARGET LuaJIT::LuaJIT PROPERTY INTERFACE_LINK_LIBRARIES "dl;m")
        endif()
    endif()
endif()

mark_as_advanced(LUAJIT_INCLUDE_DIR LUAJIT_LIBRARY)
