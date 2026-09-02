# Lua-owned reflection projection. The core runtime metadata owner does not depend on Lua.

include_guard(GLOBAL)

function(lux_script_ability_lua_projection)
    set(one_value_args TARGET OUTPUT_ROOT)
    set(multi_value_args SOURCES LOGICAL_PATHS EXTRA_COMPILE_OPTIONS)
    cmake_parse_arguments(ARGS "" "${one_value_args}" "${multi_value_args}" ${ARGN})
    if(NOT ARGS_TARGET OR NOT ARGS_SOURCES OR NOT TARGET ${ARGS_TARGET})
        message(FATAL_ERROR
            "[lux_script_ability_lua_projection] valid TARGET and explicit SOURCES are required"
        )
    endif()
    if(NOT ARGS_LOGICAL_PATHS)
        foreach(source IN LISTS ARGS_SOURCES)
            get_filename_component(logical "${source}" NAME)
            list(APPEND ARGS_LOGICAL_PATHS "${logical}")
        endforeach()
    endif()
    list(LENGTH ARGS_SOURCES source_count)
    list(LENGTH ARGS_LOGICAL_PATHS logical_count)
    if(NOT source_count EQUAL logical_count)
        message(FATAL_ERROR
            "[lux_script_ability_lua_projection] SOURCES and LOGICAL_PATHS must have equal length"
        )
    endif()

    if(NOT COMMAND lux_add_codegen_job)
        find_package(lux-cxx REQUIRED CONFIG COMPONENTS reflection_generator)
        include_component_cmake_scripts(lux::cxx::reflection_generator)
    endif()
    if(ARGS_OUTPUT_ROOT)
        set(output_root "${ARGS_OUTPUT_ROOT}")
    else()
        set(output_root "${CMAKE_CURRENT_BINARY_DIR}/generated/${ARGS_TARGET}/script_abilities_lua")
    endif()
    set(job "${ARGS_TARGET}_script_abilities_lua")
    set(template_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/template")
    lux_add_codegen_job(
        NAME ${job}
        MARKER luxability
        TARGET_FILES ${ARGS_SOURCES}
        LOGICAL_PATHS ${ARGS_LOGICAL_PATHS}
        EXTRA_COMPILE_OPTIONS -D__LUX_PARSE_TIME__=1 ${ARGS_EXTRA_COMPILE_OPTIONS}
    )
    lux_codegen_add_projection(
        JOB ${job}
        NAME script_ability_lua
        TEMPLATE ${template_dir}/script_ability_lua.template
        OUTPUT_ROOT ${output_root}
        OUTPUT_SUFFIX .ability_lua.generated.hpp
        FLAT_OUTPUT
    )
    lux_target_add_codegen(
        TARGET ${ARGS_TARGET}
        JOB ${job}
        PROJECTIONS script_ability_lua
        DONT_ADD_TO_SOURCE
    )
    target_include_directories(${ARGS_TARGET} PRIVATE "${output_root}")
endfunction()

# =============================================================================
# engine_add_lua_binding
#   Creates a meta generation target that renders meta_lua.template, producing
#   a .lua.cpp file with the _lua() sol2 binding function.
#
#   Parameters:
#     NAME                  – unique target name for this binding set
#     FROM_META             – optional runtime TypeInfo parse job to inherit
#                             TARGET_FILES, EXTRA_COMPILE_OPTIONS, and
#                             REGISTER_FUNC_NAME from
#     REGISTER_FUNC_NAME    – overrides / required when FROM_META not given
#     TARGET_FILES          – overrides TARGET_FILES from FROM_META
#     EXTRA_COMPILE_OPTIONS – extra flags (appended to FROM_META's flags)
#     REGISTER_FUNC_MACRO   – optional DLL-export macro
#     CUSTOM_INCLUDE        – optional extra header to include in output
# =============================================================================
function(engine_add_lua_binding)
    set(one_value_args
        NAME
        FROM_META
        REGISTER_FUNC_NAME
        REGISTER_FUNC_MACRO
        CUSTOM_INCLUDE
    )
    set(multi_value_args
        TARGET_FILES
        EXTRA_COMPILE_OPTIONS
    )
    cmake_parse_arguments(ARGS "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT ARGS_NAME)
        message(FATAL_ERROR "[engine_add_lua_binding] NAME is required")
    endif()

    # Inherit from FROM_META
    if(ARGS_FROM_META)
        if(NOT ARGS_TARGET_FILES)
            get_target_property(_inh_files ${ARGS_FROM_META} META_TARGET_FILES)
            if(_inh_files)
                set(ARGS_TARGET_FILES ${_inh_files})
            endif()
        endif()
        if(NOT ARGS_EXTRA_COMPILE_OPTIONS)
            get_target_property(_inh_opts ${ARGS_FROM_META} META_EXTRA_COMPILE_OPTIONS)
            if(_inh_opts)
                # Strip the -D__LUX_PARSE_TIME__=1 that engine_add_meta always
                # may prepend; the Lua projection adds it below.
                list(REMOVE_ITEM _inh_opts "-D__LUX_PARSE_TIME__=1")
                set(ARGS_EXTRA_COMPILE_OPTIONS ${_inh_opts})
            endif()
        endif()
        if(NOT ARGS_REGISTER_FUNC_NAME)
            get_target_property(_inh_reg ${ARGS_FROM_META} REGISTER_FUNC_NAME)
            if(_inh_reg)
                set(ARGS_REGISTER_FUNC_NAME ${_inh_reg})
            endif()
        endif()
    endif()

    if(NOT ARGS_REGISTER_FUNC_NAME)
        message(FATAL_ERROR "[engine_add_lua_binding] REGISTER_FUNC_NAME is required (or use FROM_META)")
    endif()

    get_filename_component(LUX_ENGINE_META_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR} DIRECTORY)
    set(_lua_template "${LUX_ENGINE_META_DIR}/template/meta_lua.template")

    if(ARGS_REGISTER_FUNC_MACRO)
        set(_json_func_macro "{\"register_function_macro\":\"${ARGS_REGISTER_FUNC_MACRO}\"}")
    else()
        set(_json_func_macro "{\"register_function_macro\":false}")
    endif()
    if(ARGS_CUSTOM_INCLUDE)
        set(_json_custom_include "{\"custom_include\":\"${ARGS_CUSTOM_INCLUDE}\"}")
    else()
        set(_json_custom_include "{\"custom_include\":false}")
    endif()

    set(_logical_paths)
    foreach(_header IN LISTS ARGS_TARGET_FILES)
        get_filename_component(_logical "${_header}" NAME)
        list(APPEND _logical_paths "${_logical}")
    endforeach()
    lux_add_codegen_job(
        NAME ${ARGS_NAME}
        MARKER luxref
        TARGET_FILES ${ARGS_TARGET_FILES}
        LOGICAL_PATHS ${_logical_paths}
        EXTRA_COMPILE_OPTIONS
            -D__LUX_PARSE_TIME__=1
            ${ARGS_EXTRA_COMPILE_OPTIONS}
    )
    lux_codegen_add_projection(
        JOB ${ARGS_NAME}
        NAME lua_binding
        TEMPLATE ${_lua_template}
        OUTPUT_ROOT ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
        OUTPUT_SUFFIX .lua.cpp
        FLAT_OUTPUT
        JSON_FIELD "{\"register_function_name\":\"${ARGS_REGISTER_FUNC_NAME}\"}"
                   "${_json_func_macro}"
                   "${_json_custom_include}"
    )

    set_target_properties(${ARGS_NAME} PROPERTIES
        REGISTER_FUNC_NAME "${ARGS_REGISTER_FUNC_NAME}"
    )
endfunction()

# =============================================================================
# engine_target_add_lua_binding
#   Links generated .lua.cpp files into TARGET and produces lua_registration.hpp
#   with an inline LuxRegisterAllMetas_LUA(sol::state_view&) aggregator.
#
#   Parameters:
#     TARGET                       – CMake target to link .lua.cpp files into
#     LUA_BINDINGS                 – list of engine_add_lua_binding target names
#     OUT_DIR                      – output directory for lua_registration.hpp
#     TOP_LEVEL_REGISTER_FUNC_PREFIX – prefix for the generated inline function
#                                    (default: LuxRegisterAllMetas)
# =============================================================================
function(engine_target_add_lua_binding)
    set(one_value_args TARGET OUT_DIR TOP_LEVEL_REGISTER_FUNC_PREFIX OUTPUT_FILENAME)
    set(multi_value_args LUA_BINDINGS)
    cmake_parse_arguments(ARGS "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT ARGS_TARGET)
        message(FATAL_ERROR "[engine_target_add_lua_binding] TARGET is required")
    endif()
    if(NOT ARGS_LUA_BINDINGS)
        message(FATAL_ERROR "[engine_target_add_lua_binding] LUA_BINDINGS is required")
    endif()
    if(NOT ARGS_OUT_DIR)
        message(FATAL_ERROR "[engine_target_add_lua_binding] OUT_DIR is required")
    endif()

    foreach(binding ${ARGS_LUA_BINDINGS})
        lux_target_add_codegen(
            TARGET ${ARGS_TARGET}
            JOB ${binding}
            PROJECTIONS lua_binding
        )
    endforeach()

    set(REGISTER_FUNC_NAMES "")
    foreach(binding IN LISTS ARGS_LUA_BINDINGS)
        get_target_property(reg_func_name ${binding} REGISTER_FUNC_NAME)
        if(reg_func_name)
            list(APPEND REGISTER_FUNC_NAMES ${reg_func_name})
        else()
            message(WARNING "[engine_target_add_lua_binding] No REGISTER_FUNC_NAME for: ${binding}")
        endif()
    endforeach()

    set(_externs   "")
    set(_sol_calls "")
    foreach(f IN LISTS REGISTER_FUNC_NAMES)
        set(_externs   "${_externs}\nvoid ${f}_lua(sol::state_view&);")
        set(_sol_calls "${_sol_calls}\n    ${f}_lua(state);")
    endforeach()

    if(ARGS_TOP_LEVEL_REGISTER_FUNC_PREFIX)
        set(REGISTER_FUNC_PREFIX "${ARGS_TOP_LEVEL_REGISTER_FUNC_PREFIX}")
    else()
        set(REGISTER_FUNC_PREFIX "LuxRegisterAllMetas")
    endif()

    set(EXTERN_PROTOTYPES   "${_externs}")
    set(SOL_CALL_STATEMENTS "${_sol_calls}")

    if(NOT ARGS_OUTPUT_FILENAME)
        set(ARGS_OUTPUT_FILENAME "lua_registration.hpp")
    endif()

    get_filename_component(LUX_ENGINE_META_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR} DIRECTORY)
    set(_template_file "${LUX_ENGINE_META_DIR}/template/lua_registration.hpp.in")
    set(_output_hpp    "${ARGS_OUT_DIR}/${ARGS_OUTPUT_FILENAME}")

    configure_file(${_template_file} ${_output_hpp} @ONLY)

    target_include_directories(${ARGS_TARGET} PRIVATE ${ARGS_OUT_DIR})
    get_filename_component(_output_name "${_output_hpp}" NAME)
    message(STATUS "[meta-lua] ${ARGS_TARGET} -> ${_output_name}")
endfunction()
