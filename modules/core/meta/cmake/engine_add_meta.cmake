# ===========================================
# Include guard
# ===========================================
if(_LUX_ENGINE_META_TOOLS_INCLUDED_)
  return()
endif()
set(_LUX_ENGINE_META_TOOLS_INCLUDED_ TRUE)

find_package(lux-cxx REQUIRED CONFIG COMPONENTS reflection_generator)
include_component_cmake_scripts(lux::cxx::reflection_generator)

# Locate the generator once at include time; find_program result is cached so
# subsequent calls inside functions are instant and no message is repeated.
find_program(LUX_META_GENERATOR lux_meta_generator REQUIRED)
if(NOT LUX_META_GENERATOR)
    message(FATAL_ERROR "[lux-meta] Could not find 'lux_meta_generator' executable")
endif()
if(NOT _LUX_META_GENERATOR_MSG_SHOWN)
    get_filename_component(_gen_name "${LUX_META_GENERATOR}" NAME)
    message(STATUS "[lux-meta] Using generator: ${_gen_name}")
    set(_LUX_META_GENERATOR_MSG_SHOWN TRUE CACHE INTERNAL "")
endif()

function(engine_add_meta)
    set(one_value_args
        NAME
        SOURCE_FILE
        REGISTER_FUNC_NAME
        REGISTER_FUNC_MACRO
        CUSTOM_INCLUDE
    )
    set(multi_value_args
        TARGET_FILES
        EXTRA_COMPILE_OPTIONS
    )
    set(optional_args ECHO ALWAYS_REGENERATE)
    cmake_parse_arguments(ARGS "${optional_args}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT ARGS_NAME)
        message(FATAL_ERROR "[add_meta] NAME parameter is required")
    endif()
    if(NOT ARGS_REGISTER_FUNC_NAME)
        message(FATAL_ERROR "[add_meta] REGISTER_FUNC_NAME parameter is required")
    endif()

    # Set default values
    if(NOT ARGS_SOURCE_FILE)
        set(ARGS_SOURCE_FILE "")  # If empty, please supply compile options via EXTRA_COMPILE_OPTIONS.
    endif()

    get_filename_component(LUX_ENGINE_META_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR} DIRECTORY)
    set(LUX_ENGINE_DEFAULT_META_TEMPLATE "${LUX_ENGINE_META_DIR}/template/meta_reflect.template")
    # Store configuration in target properties (prefix properties with META_)
    
    if(ARGS_REGISTER_FUNC_MACRO)
        set(_json_register_func_macro "{\"register_function_macro\":\"${ARGS_REGISTER_FUNC_MACRO}\"}")
    else()
        set(_json_register_func_macro "{\"register_function_macro\":false}")
    endif()
    if(ARGS_CUSTOM_INCLUDE)
        set(_json_custom_include "{\"custom_include\":\"${ARGS_CUSTOM_INCLUDE}\"}")
    else()
        set(_json_custom_include "{\"custom_include\":false}")
    endif()

    add_meta(
        NAME                    ${ARGS_NAME}
        MARKER                  "luxref"
        TEMPLATE                ${LUX_ENGINE_DEFAULT_META_TEMPLATE}
        OUT_DIR                 ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
        META_SUFFIX             ".meta.cpp"
        SERIAL_META             ${ARGS_SOURCE_FILE}
        TARGET_FILES            ${ARGS_TARGET_FILES}
        EXTRA_COMPILE_OPTIONS   -D__LUX_PARSE_TIME__=1 ${ARGS_EXTRA_COMPILE_OPTIONS}
        JSON_FIELD              "{\"register_function_name\":\"${ARGS_REGISTER_FUNC_NAME}\"}"
                                "${_json_register_func_macro}"
                                "${_json_custom_include}"
    )

    set_target_properties(${ARGS_NAME} PROPERTIES
        REGISTER_FUNC_NAME      "${ARGS_REGISTER_FUNC_NAME}"
    )
endfunction()

function(engine_target_add_meta)
    set(one_value_args TARGET OUT_DIR TOP_LEVEL_REGISTER_FUNC_PREFIX CONFIG_FILE OUTPUT_FILENAME)
    set(multi_value_args METAS)
    cmake_parse_arguments(ARGS "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT ARGS_TARGET)
        message(FATAL_ERROR "[engine_target_add_meta] TARGET parameter is required")
    endif()
    if(NOT ARGS_METAS)
        message(FATAL_ERROR "[engine_target_add_meta] METAS parameter is required")
    endif()
    if(NOT ARGS_OUT_DIR)
        message(FATAL_ERROR "[engine_target_add_meta] OUT_DIR parameter is required")
    endif()

    foreach(meta ${ARGS_METAS})
        target_add_meta(NAME ${meta} TARGET ${ARGS_TARGET})
    endforeach()
    list(JOIN ARGS_METAS ", " _meta_list_str)
    message(STATUS "[meta] ${ARGS_TARGET} <- ${_meta_list_str}")

    set(REGISTER_FUNC_NAMES "")
    foreach(meta IN LISTS ARGS_METAS)
        get_target_property(reg_func_name ${meta} REGISTER_FUNC_NAME)
        if(reg_func_name)
            list(APPEND REGISTER_FUNC_NAMES ${reg_func_name})
        else()
            message(WARNING "[engine_target_add_meta] No REGISTER_FUNC_NAME found for meta target: ${meta}")
        endif()
    endforeach()

    set(_externs "")
    set(_calls   "")
    foreach(f IN LISTS REGISTER_FUNC_NAMES)
        set(_externs "${_externs}\nvoid ${f}_meta(lux::meta::ReflectionRegistry&, lux::meta::qual_type_index_fix_list&);")
        set(_calls   "${_calls}\n    ${f}_meta(registry, fix_list);")
    endforeach()

    if(ARGS_TOP_LEVEL_REGISTER_FUNC_PREFIX)
        set(REGISTER_FUNC_PREFIX "${ARGS_TOP_LEVEL_REGISTER_FUNC_PREFIX}")
    else()
        set(REGISTER_FUNC_PREFIX "LuxRegisterAllMetas")
    endif()

    set(EXTERN_PROTOTYPES    "${_externs}")
    set(META_CALL_STATEMENTS "${_calls}")

    if(NOT ARGS_OUTPUT_FILENAME)
        set(ARGS_OUTPUT_FILENAME "meta_registration.hpp")
    endif()

    get_filename_component(LUX_ENGINE_META_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR} DIRECTORY)
    set(REGISTER_TEMPLATE_FILE "${LUX_ENGINE_META_DIR}/template/meta_registration.hpp.in")
    set(_output_hpp "${ARGS_OUT_DIR}/${ARGS_OUTPUT_FILENAME}")

    configure_file(
        ${REGISTER_TEMPLATE_FILE}
        ${_output_hpp}
        @ONLY
    )

    target_include_directories(
        ${ARGS_TARGET}
        PRIVATE
        ${ARGS_OUT_DIR}
    )
    get_filename_component(_output_name "${_output_hpp}" NAME)
    message(STATUS "[meta] ${ARGS_TARGET} -> ${_output_name}")
endfunction()

# =============================================================================
# engine_add_lua_binding
#   Creates a meta generation target that renders meta_lua.template, producing
#   a .lua.cpp file with the _lua() sol2 binding function.
#
#   Parameters:
#     NAME                  – unique target name for this binding set
#     FROM_META             – (optional) engine_add_meta target to inherit
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
                # prepends; engine_add_lua_binding will re-add it below.
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

    add_meta(
        NAME                    ${ARGS_NAME}
        MARKER                  "luxref"
        TEMPLATE                ${_lua_template}
        OUT_DIR                 ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
        META_SUFFIX             ".lua.cpp"
        TARGET_FILES            ${ARGS_TARGET_FILES}
        EXTRA_COMPILE_OPTIONS   -D__LUX_PARSE_TIME__=1 ${ARGS_EXTRA_COMPILE_OPTIONS}
        JSON_FIELD              "{\"register_function_name\":\"${ARGS_REGISTER_FUNC_NAME}\"}"
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
        target_add_meta(NAME ${binding} TARGET ${ARGS_TARGET})
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

# =============================================================================
# engine_enable_module_meta
#   Single-call helper for lux-engine modules.  Creates one engine_add_meta
#   target per header, links them all into TARGET, and exports the aggregator
#   header through component_include_directories.
#
#   The DLL-export macro (e.g. LUX_FUNCTION_PUBLIC) and its companion include
#   (lux/engine/function/visibility.h) are auto-derived from the layer sub-path
#   detected in CMAKE_CURRENT_SOURCE_DIR:  .../modules/<layer>/...
#
#   Parameters:
#     TARGET        – CMake target to attach generated code to        (required)
#     TARGET_FILES  – list of annotated header files to reflect        (required)
#     LUA_BINDING   – flag: also generate and link Lua sol2 bindings   (optional)
# =============================================================================
function(engine_enable_module_meta)
    set(options       LUA_BINDING)
    set(one_value_args TARGET SIDECAR_TARGET)
    set(multi_value_args TARGET_FILES)
    cmake_parse_arguments(ARGS "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT ARGS_TARGET)
        message(FATAL_ERROR "[engine_enable_module_meta] TARGET is required")
    endif()
    if(NOT ARGS_TARGET_FILES)
        message(FATAL_ERROR "[engine_enable_module_meta] TARGET_FILES is required")
    endif()

    # When SIDECAR_TARGET is provided, the generated .meta.cpp (and .lua.cpp)
    # sources, the publish of the aggregator include directory, and the
    # /INCLUDE: linker anchors all attach to the sidecar instead of TARGET.
    # This is the "editor-only reflection" model — runtime modules ship slim;
    # the editor pulls in the sidecar DLL which carries every reflection
    # registrar static-init.
    #
    # When SIDECAR_TARGET is omitted, behaviour is unchanged: meta sources
    # live in TARGET itself, just like before this commit.
    if(ARGS_SIDECAR_TARGET)
        if(NOT TARGET ${ARGS_SIDECAR_TARGET})
            message(FATAL_ERROR
                "[engine_enable_module_meta] SIDECAR_TARGET '${ARGS_SIDECAR_TARGET}' "
                "must be created by the caller before this call (e.g. "
                "`add_library(${ARGS_SIDECAR_TARGET} SHARED)`).")
        endif()
        set(_meta_owner_target ${ARGS_SIDECAR_TARGET})
    else()
        set(_meta_owner_target ${ARGS_TARGET})
    endif()

    # --- auto-derive visibility macro and include from path layout -----------
    # Expect:  .../modules/<layer>/...
    string(REPLACE "\\" "/" _src_dir_fwd "${CMAKE_CURRENT_SOURCE_DIR}")
    string(REGEX MATCH "/modules/([^/]+)/" _dummy "${_src_dir_fwd}/")
    set(_layer "${CMAKE_MATCH_1}")
    if(NOT _layer)
        set(_layer "function")  # safe fallback
    endif()
    string(TOUPPER "${_layer}" _UPPER_LAYER)
    set(_func_macro  "LUX_${_UPPER_LAYER}_PUBLIC")
    set(_vis_include "lux/engine/${_layer}/visibility.h")

    # --- derive output names from target slug --------------------------------
    set(_slug "${ARGS_TARGET}")
    # CamelCase first letter for the aggregator prefix (e.g. "gameplay" → "Gameplay")
    string(SUBSTRING "${_slug}" 0 1 _first)
    string(TOUPPER   "${_first}" _first_upper)
    string(LENGTH    "${_slug}"  _len)
    math(EXPR        _rest_len "${_len} - 1")
    string(SUBSTRING "${_slug}" 1 ${_rest_len} _rest)
    set(_camel "${_first_upper}${_rest}")

    set(_out_dir         "${CMAKE_CURRENT_BINARY_DIR}/include")
    set(_meta_out_hpp    "${_slug}_meta_registration.hpp")
    set(_lua_out_hpp     "${_slug}_lua_registration.hpp")
    set(_meta_prefix     "LuxRegister${_camel}Metas")

    # --- one engine_add_meta (+ optional lua) per header ---------------------
    set(_meta_targets "")
    set(_lua_targets  "")

    foreach(_header IN LISTS ARGS_TARGET_FILES)
        get_filename_component(_stem "${_header}" NAME_WE)

        # Convert CamelCase stem to snake_case (e.g. TransformComponent → transform_component)
        string(REGEX REPLACE "([A-Z])" "_\\1" _snake "${_stem}")
        string(TOLOWER "${_snake}" _snake)
        string(REGEX REPLACE "^_" "" _snake "${_snake}")  # strip any leading underscore

        set(_meta_name "${_slug}_${_snake}_meta")
        set(_func_name "${_slug}_${_snake}")
        set(_lua_name  "${_slug}_${_snake}_lua")

        engine_add_meta(
            NAME                ${_meta_name}
            REGISTER_FUNC_NAME  ${_func_name}
            REGISTER_FUNC_MACRO ${_func_macro}
            CUSTOM_INCLUDE      "${_vis_include}"
            TARGET_FILES        ${_header}
        )
        list(APPEND _meta_targets ${_meta_name})

        if(ARGS_LUA_BINDING)
            # The export macro + visibility include must flow to the lua
            # template too: in the SIDECAR model the generated _lua()
            # registration functions compile into a SHARED library and are
            # called from the host executable — without dllexport they never
            # appear in the import lib (LNK2019 at the consumer link).
            engine_add_lua_binding(
                NAME                ${_lua_name}
                FROM_META           ${_meta_name}
                REGISTER_FUNC_MACRO ${_func_macro}
                CUSTOM_INCLUDE      "${_vis_include}"
            )
            list(APPEND _lua_targets ${_lua_name})
        endif()
    endforeach()

    # --- link meta objects into the owner target, generate aggregator header
    #     (owner is SIDECAR_TARGET when given, TARGET otherwise — see above).
    engine_target_add_meta(
        TARGET                         ${_meta_owner_target}
        METAS                          ${_meta_targets}
        OUT_DIR                        ${_out_dir}
        OUTPUT_FILENAME                ${_meta_out_hpp}
        TOP_LEVEL_REGISTER_FUNC_PREFIX ${_meta_prefix}
    )

    if(ARGS_LUA_BINDING AND _lua_targets)
        engine_target_add_lua_binding(
            TARGET                         ${_meta_owner_target}
            LUA_BINDINGS                   ${_lua_targets}
            OUT_DIR                        ${_out_dir}
            OUTPUT_FILENAME                ${_lua_out_hpp}
            TOP_LEVEL_REGISTER_FUNC_PREFIX ${_meta_prefix}
        )
    endif()

    # --- export generated headers as part of the owner target's public
    #     build-time API. Consumers that link the owner (editor links the
    #     sidecar; legacy modules link themselves) see the aggregator header.
    #
    # The sidecar path uses plain target_include_directories because the
    # sidecar is a raw add_library, not an add_component() target — it has
    # no COMPONENT_TARGET property so component_include_directories() would
    # FATAL_ERROR. The non-sidecar path keeps the component-aware publish so
    # the include dir participates in the standard component export flow.
    if(ARGS_SIDECAR_TARGET)
        target_include_directories(${_meta_owner_target}
            PUBLIC "$<BUILD_INTERFACE:${_out_dir}>")
    else()
        component_include_directories(
            ${_meta_owner_target}
            BUILD_TIME_EXPORT
                ${_out_dir}
        )
    endif()

    # --- propagate /INCLUDE linker anchors to all consumers ------------------
    # On Windows a DLL is loaded only when at least one of its exported symbols
    # is directly referenced by the consumer.  Modules whose public API is
    # entirely header-only (e.g. plain structs annotated with LUX_CLASS() but
    # no exported methods) may never generate a symbol reference, so the
    # MetaModuleRegistrar static instances inside their generated .meta.cpp
    # files never run and no types are registered.
    #
    # We fix this by publishing INTERFACE_LINK_OPTIONS on the owner so that
    # any executable (or DLL) that does target_link_libraries(...  OWNER) gets
    # a /INCLUDE:<func>_meta flag for every exported meta function.  That
    # forces the MSVC linker to pull the import-lib entry in, which adds the
    # owner's DLL to the consumer's PE import table and ensures the DLL is
    # loaded at startup — making all MetaModuleRegistrar statics fire
    # automatically.
    foreach(_mt IN LISTS _meta_targets)
        get_target_property(_anchor ${_mt} REGISTER_FUNC_NAME)
        if(_anchor)
            # MSVC: /INCLUDE:symbol forces the import-lib entry to be pulled in.
            # GNU ld (Linux/ELF): -u symbol  achieves the same.
            # Apple ld (macOS/Mach-O): C symbols have a leading '_' in the object
            # file, so the undefined-symbol flag must include it.
            set_property(TARGET ${_meta_owner_target} APPEND PROPERTY
                INTERFACE_LINK_OPTIONS
                "$<$<CXX_COMPILER_ID:MSVC>:/INCLUDE:${_anchor}_meta>"
                "$<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<NOT:$<PLATFORM_ID:Darwin>>>:-Wl,-u,${_anchor}_meta>"
                "$<$<AND:$<NOT:$<CXX_COMPILER_ID:MSVC>>,$<PLATFORM_ID:Darwin>>:-Wl,-u,_${_anchor}_meta>"
            )
        endif()
    endforeach()
endfunction()