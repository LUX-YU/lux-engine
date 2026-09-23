include_guard(GLOBAL)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

string(TOUPPER "${CMAKE_BUILD_TYPE}" _lux_plugin_configuration)
set(_lux_plugin_environment "${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_CXX_COMPILER_ARCHITECTURE_ID};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_SIZEOF_VOID_P};${CMAKE_BUILD_TYPE};${CMAKE_CXX_STANDARD};${CMAKE_MSVC_RUNTIME_LIBRARY};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_${_lux_plugin_configuration}}")

# Installed SDKs carry this value; consumers never inspect the engine source tree.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/LuxPluginSdk.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/LuxPluginSdk.cmake")
    if(NOT _lux_plugin_environment STREQUAL LUX_PLUGIN_SDK_ENVIRONMENT)
        message(FATAL_ERROR "Plugin compiler, architecture, runtime or configuration differs from the installed SDK")
    endif()
else()
    file(GLOB_RECURSE _lux_sdk_headers CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/modules/*/include/*.hpp" "${PROJECT_SOURCE_DIR}/engine/*/include/*.hpp")
    list(SORT _lux_sdk_headers)
    set(_lux_sdk_contract "${_lux_plugin_environment}")
    foreach(_header IN LISTS _lux_sdk_headers)
        file(SHA256 "${_header}" _hash)
        string(APPEND _lux_sdk_contract ";${_hash}")
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_lux_sdk_headers})
    string(SHA256 LUX_PLUGIN_SDK_ABI "${_lux_sdk_contract}")
    file(WRITE "${CMAKE_BINARY_DIR}/LuxPluginSdk.cmake"
        "set(LUX_PLUGIN_SDK_ABI \"${LUX_PLUGIN_SDK_ABI}\")\nset(LUX_PLUGIN_SDK_ENVIRONMENT [==[${_lux_plugin_environment}]==])\n")
    install(FILES "${CMAKE_BINARY_DIR}/LuxPluginSdk.cmake" "${CMAKE_CURRENT_LIST_FILE}"
        DESTINATION share/lux-engine/plugins)
    install(FILES "${CMAKE_CURRENT_LIST_DIR}/plugin_descriptor.py" "${CMAKE_CURRENT_LIST_DIR}/plugin_capabilities.py"
        DESTINATION share/lux-engine/plugins)
endif()

function(lux_add_plugin_exports)
    cmake_parse_arguments(P "" "TARGET;DESCRIPTION;EDITOR_TARGET;MODULE_ID" "INPUTS" ${ARGN})
    if(NOT TARGET "${P_TARGET}" OR NOT P_DESCRIPTION OR NOT P_INPUTS)
        message(FATAL_ERROR "lux_add_plugin_exports requires TARGET, DESCRIPTION and INPUTS")
    endif()
    if(P_MODULE_ID)
        set(_module "${P_MODULE_ID}")
    else()
        file(READ "${P_DESCRIPTION}" _declaration)
        string(JSON _module GET "${_declaration}" plugin id)
    endif()
    set(_directory "${CMAKE_CURRENT_BINARY_DIR}/plugin/${P_TARGET}")
    set(_identity "${_directory}/identity.cpp")
    set(_descriptor "${CMAKE_BINARY_DIR}/share/lux-engine/plugins/${_module}.json")
    set(_editor_arguments)
    if(P_EDITOR_TARGET)
        list(APPEND _editor_arguments --editor-library "bin/$<TARGET_FILE_NAME:${P_EDITOR_TARGET}>")
        target_sources(${P_EDITOR_TARGET} PRIVATE "${_identity}")
        target_link_libraries(${P_EDITOR_TARGET} PRIVATE lux::engine::platform::dynamic_library)
    endif()
    get_property(_value_fragments GLOBAL PROPERTY LUX_VALUE_METADATA_FRAGMENTS)
    set(_value_arguments)
    if(_value_fragments)
        set(_value_arguments --value-fragments ${_value_fragments})
    endif()
    add_custom_command(OUTPUT "${_identity}" "${_descriptor}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/plugin_descriptor.py"
            --input "${P_DESCRIPTION}" --identity "${_identity}" --output "${_descriptor}"
            --library "bin/$<TARGET_FILE_NAME:${P_TARGET}>" --sdk-abi "${LUX_PLUGIN_SDK_ABI}"
            ${_editor_arguments} --sources ${P_INPUTS} ${_value_arguments}
        DEPENDS "${P_DESCRIPTION}" ${P_INPUTS} ${_value_fragments} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/plugin_descriptor.py"
        VERBATIM)
    target_sources(${P_TARGET} PRIVATE "${_identity}")
    target_link_libraries(${P_TARGET} PRIVATE lux::engine::platform::dynamic_library)
    set_property(TARGET ${P_TARGET} PROPERTY LUX_PLUGIN_DESCRIPTION "${_descriptor}")
    set_property(GLOBAL APPEND PROPERTY LUX_PLUGIN_DESCRIPTIONS "${_descriptor}")
    install(FILES "${_descriptor}" DESTINATION share/lux-engine/plugins)
endfunction()

function(lux_generate_capabilities)
    cmake_parse_arguments(P "" "BASE" "" ${ARGN})
    get_property(descriptions GLOBAL PROPERTY LUX_PLUGIN_DESCRIPTIONS)
    set(output "${CMAKE_BINARY_DIR}/share/lux-engine/editor/capabilities.json")
    set(base_arguments)
    if(P_BASE)
        set(base_arguments --base "${P_BASE}")
    endif()
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/plugin_capabilities.py"
            --output "${output}" --inputs ${descriptions} ${base_arguments}
        DEPENDS ${descriptions} ${P_BASE} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/plugin_capabilities.py" VERBATIM)
    add_custom_target(lux_plugin_capabilities ALL DEPENDS "${output}")
    install(FILES "${output}" DESTINATION share/lux-engine/editor)
endfunction()
