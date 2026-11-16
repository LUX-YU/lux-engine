include_guard(GLOBAL)

get_filename_component(lux_shader_repo "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
file(READ "${lux_shader_repo}/bootstrap/shader-tools.json" lux_shader_lock)
string(JSON lux_slang_version GET "${lux_shader_lock}" slang version)
string(JSON lux_dxc_version GET "${lux_shader_lock}" dxc version)
set(LUX_SHADER_COMPILER_VERSION "slang-${lux_slang_version}+dxc-${lux_dxc_version}")
set(LUX_SHADER_TOOL_CACHE "${lux_shader_repo}/../build/dependencies" CACHE PATH "Shared fixed shader tools")

function(lux_shader_tool name output)
    string(JSON version GET "${lux_shader_lock}" "${name}" version)
    string(JSON url GET "${lux_shader_lock}" "${name}" url)
    string(JSON digest GET "${lux_shader_lock}" "${name}" sha256)
    set(directory "${LUX_SHADER_TOOL_CACHE}/${name}-${version}")
    if(NOT EXISTS "${directory}")
        if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
            message(FATAL_ERROR "Provide the pinned ${name} ${version} host package in ${directory}")
        endif()
        file(MAKE_DIRECTORY "${LUX_SHADER_TOOL_CACHE}")
        set(archive "${LUX_SHADER_TOOL_CACHE}/${name}-${version}.zip")
        file(DOWNLOAD "${url}" "${archive}" EXPECTED_HASH "SHA256=${digest}" TLS_VERIFY ON)
        file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${directory}")
        file(REMOVE "${archive}")
    endif()
    set(${output} "${directory}" PARENT_SCOPE)
endfunction()

lux_shader_tool(slang LUX_SLANG_ROOT)
lux_shader_tool(dxc LUX_DXC_ROOT)

find_package(slang "${lux_slang_version}" EXACT CONFIG REQUIRED PATHS "${LUX_SLANG_ROOT}/cmake" NO_DEFAULT_PATH)

if(WIN32)
    set(LUX_SHADER_COMPILER_RUNTIMES
        "${LUX_SLANG_ROOT}/bin/slang-compiler.dll"
        "${LUX_SLANG_ROOT}/bin/slang-glslang.dll"
        "${LUX_SLANG_ROOT}/bin/slang-glsl-module.dll"
        "${LUX_DXC_ROOT}/bin/x64/dxcompiler.dll"
        "${LUX_DXC_ROOT}/bin/x64/dxil.dll"
    )
    foreach(runtime IN LISTS LUX_SHADER_COMPILER_RUNTIMES)
        if(NOT EXISTS "${runtime}")
            message(FATAL_ERROR "Pinned shader runtime missing: ${runtime}")
        endif()
    endforeach()
    install(FILES ${LUX_SHADER_COMPILER_RUNTIMES} DESTINATION bin COMPONENT lux_engine_resource)
endif()

install(FILES "${LUX_SLANG_ROOT}/LICENSE" DESTINATION share/licenses/slang COMPONENT lux_engine_resource)
install(
    FILES "${LUX_DXC_ROOT}/LICENSE-LLVM.txt" "${LUX_DXC_ROOT}/LICENSE-MIT.txt" "${LUX_DXC_ROOT}/LICENSE-MS.txt"
    DESTINATION share/licenses/dxc
    COMPONENT lux_engine_resource
)
