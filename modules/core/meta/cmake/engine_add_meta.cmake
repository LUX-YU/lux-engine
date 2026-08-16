# ===========================================
# Include guard
# ===========================================
if(_LUX_ENGINE_META_TOOLS_INCLUDED_)
  return()
endif()
set(_LUX_ENGINE_META_TOOLS_INCLUDED_ TRUE)

# 反射有三样东西共用一个名字,交叉编译时它们的去向不同:
#   · 生成器 lux_meta_generator(吃 libclang)—— **只跑在宿主**,不为目标构建
#   · add_meta 这套 cmake 脚本            —— 两边都要,它定义 codegen 规则
#   · 运行时注册表                          —— 目标侧要,约 1.2 MB,与 libclang 无关
#
# 顶层 CMakeLists 出于第一条,在 if(CMAKE_CROSSCOMPILING) 时不请求
# reflection_generator 组件;但**这里不能跟着分岔**。试过了:改成交叉编译只请求
# compile_time,下一行的 include_component_cmake_scripts 就拿不到组件目录,
# add_meta 直接不可用(component_assets.cmake 报 get_target_property 失败)。
# 定义 codegen 规则的脚本和吃 libclang 的生成器目标打包在同一个组件里,拆不开。
#
# 交叉构建直接读取宿主安装前缀中的纯 CMake 脚本；不能 find target 架构的
# reflection_generator 组件，否则它会无意义地寻找 target libclang，并把宿主
# executable 与目标依赖前缀重新混在一起。
if(CMAKE_CROSSCOMPILING)
    if(NOT COMMAND lux_find_host_program)
        message(FATAL_ERROR
            "[lux-meta] Cross builds require lux_find_host_program"
        )
    endif()
    lux_find_host_program(
        LUX_META_GENERATOR_EXECUTABLE
        REQUIRED
        NAMES lux_meta_generator
    )
    set(_lux_cxx_meta_script
        "${LUX_HOST_TOOLS_PREFIX}/share/lux-cxx/reflection_generator/cmake_scripts/meta.cmake"
    )
    if(NOT EXISTS "${_lux_cxx_meta_script}")
        message(FATAL_ERROR
            "[lux-meta] Host reflection CMake script is missing: "
            "${_lux_cxx_meta_script}"
        )
    endif()
    include("${_lux_cxx_meta_script}")
else()
    find_package(lux-cxx REQUIRED CONFIG COMPONENTS reflection_generator)
    include_component_cmake_scripts(lux::cxx::reflection_generator)
endif()

# Locate the generator once at include time. Cross builds must use the explicit
# host-tools prefix; otherwise a target-architecture executable can be selected
# from CMAKE_PREFIX_PATH and fail only when the custom command runs.
if(COMMAND lux_find_host_program)
    lux_find_host_program(
        LUX_META_GENERATOR
        NAMES lux_meta_generator
    )
elseif(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR
        "[lux-meta] Cross builds require the lux_find_host_program adapter."
    )
else()
    find_program(LUX_META_GENERATOR lux_meta_generator)
endif()
if(NOT LUX_META_GENERATOR)
    # 这是**交叉编译的隐性前置条件**,以前只表现为一句 find_program REQUIRED 的
    # 裸错误。生成器是宿主工具:目标侧的构建也要调它(在宿主上跑,产出目标要编的
    # .meta.cpp),所以交叉编译之前宿主侧必须先 install 过一次。
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR
            "[lux-meta] 找不到宿主工具 lux_meta_generator。\n"
            "交叉编译依赖它:反射代码在**宿主**上生成,目标侧只编生成结果。\n"
            "先把宿主构建装好,再配置这个目标:\n"
            "    cmake -S <lux-cxx> -B <host-build> -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo\n"
            "    cmake --build <host-build> --target install -- -j 4\n"
            "然后通过 LUX_HOST_TOOLS_PREFIX 指向该宿主 install 前缀\n"
            "(当前为:${LUX_HOST_TOOLS_PREFIX})。")
    else()
        message(FATAL_ERROR
            "[lux-meta] 找不到 lux_meta_generator。它由 lux-cxx 的 "
            "reflection_generator 组件提供 —— 先构建并安装 lux-cxx。")
    endif()
endif()
if(NOT _LUX_META_GENERATOR_MSG_SHOWN)
    get_filename_component(_gen_name "${LUX_META_GENERATOR}" NAME)
    message(STATUS "[lux-meta] Using generator: ${_gen_name}")
    set(_LUX_META_GENERATOR_MSG_SHOWN TRUE CACHE INTERNAL "")
endif()

# lux-cxx 0.64 writes every generated metadata JSON through file(WRITE/APPEND)
# during each CMake configure. Even when its contents are identical, that makes
# the config newer than the generated C++/headers and turns an unrelated CMake
# edit into hundreds of false-positive codegen and compile jobs.
#
# Keep that dependency implementation behind the engine adapter. We temporarily
# move the previous config aside (a rename preserves its timestamp), let
# target_add_meta produce the candidate, and restore the previous file only when
# both contents are identical. A real option/header/template change remains
# visible to Ninja because the new config is retained; source and template files
# also remain direct custom-command dependencies.
function(_engine_target_add_meta_content_stable)
    cmake_parse_arguments(ARGS "" "NAME;TARGET" "" ${ARGN})
    if(NOT ARGS_NAME OR NOT TARGET ${ARGS_NAME})
        message(FATAL_ERROR
            "[_engine_target_add_meta_content_stable] NAME must identify a meta target")
    endif()

    get_target_property(_out_dir ${ARGS_NAME} META_OUT_DIR)
    set(_config "${_out_dir}/${ARGS_NAME}_meta_config.json")
    set(_previous "${_config}.lux-previous")

    # Recover cleanly if configuration was interrupted between the two renames.
    if(EXISTS "${_previous}" AND NOT EXISTS "${_config}")
        file(RENAME "${_previous}" "${_config}")
    elseif(EXISTS "${_previous}")
        file(REMOVE "${_previous}")
    endif()

    if(EXISTS "${_config}")
        file(RENAME "${_config}" "${_previous}")
    endif()

    target_add_meta(${ARGN})

    if(EXISTS "${_previous}" AND EXISTS "${_config}")
        file(SHA256 "${_previous}" _previous_hash)
        file(SHA256 "${_config}" _candidate_hash)
        if(_previous_hash STREQUAL _candidate_hash)
            file(REMOVE "${_config}")
            file(RENAME "${_previous}" "${_config}")
        else()
            file(REMOVE "${_previous}")
        endif()
    endif()
endfunction()

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
        _engine_target_add_meta_content_stable(NAME ${meta} TARGET ${ARGS_TARGET})
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
        _engine_target_add_meta_content_stable(NAME ${binding} TARGET ${ARGS_TARGET})
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
# engine_add_pass_params
#   Renders the two PassParams artifacts from ONE annotated header (marker
#   family `luxpass` — see MetaAnnotations.hpp):
#     <Stem>.pass.hpp  C++ side: PC layout constants + offset static_asserts,
#                      declareGraphIO / createTransientDS / pushScalars
#     <Stem>.lglslh    GLSL side: local uniforms (declaration order) + PC block
#   Both land in ${CMAKE_CURRENT_BINARY_DIR}/pass_gen, which is added to
#   TARGET's private include path. The .lglslh must additionally be routed
#   through emit_lglsl_shaders (a second call with SOURCE_DIR = pass_gen) so
#   the shader emitter injects set/binding before glslc sees it.
#
#   The luxpass marker is disjoint from luxref on purpose: the params struct
#   (holding RG/Vulkan handles) stays invisible to editor reflection; the
#   scalars struct carries BOTH markers when it also feeds the inspector.
#
#   Parameters:
#     NAME                  – unique base name for the two meta targets
#     TARGET                – consumer target (dependency + include path)
#     TARGET_FILES          – annotated header(s); one params struct per header
#     EXTRA_COMPILE_OPTIONS – extra parse flags (appended after parse-time flag)
#     OUT_DIR_VAR           – [out, optional] variable receiving the pass_gen dir
# =============================================================================
function(engine_add_pass_params)
    set(one_value_args NAME TARGET OUT_DIR_VAR)
    set(multi_value_args TARGET_FILES EXTRA_COMPILE_OPTIONS)
    cmake_parse_arguments(ARGS "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT ARGS_NAME)
        message(FATAL_ERROR "[engine_add_pass_params] NAME is required")
    endif()
    if(NOT ARGS_TARGET)
        message(FATAL_ERROR "[engine_add_pass_params] TARGET is required")
    endif()
    if(NOT ARGS_TARGET_FILES)
        message(FATAL_ERROR "[engine_add_pass_params] TARGET_FILES is required")
    endif()

    get_filename_component(LUX_ENGINE_META_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR} DIRECTORY)
    set(_hpp_template   "${LUX_ENGINE_META_DIR}/template/pass_params_hpp.template")
    set(_glslh_template "${LUX_ENGINE_META_DIR}/template/pass_params_glslh.template")
    set(_out_dir        "${CMAKE_CURRENT_BINARY_DIR}/pass_gen")

    # PARSE_INCLUDED_MARKED:scalars 结构可以住在别的头(如编辑器反射的
    # 安装头)里,经 include 对生成器可见 —— 不必与 params 结构同文件。
    # 对注册型模板(luxref)保持关闭;此处模板只按 luxpass 标记选取,安全。
    add_meta(
        NAME                    ${ARGS_NAME}_pass_hpp
        MARKER                  "luxpass"
        TEMPLATE                ${_hpp_template}
        OUT_DIR                 ${_out_dir}
        META_SUFFIX             ".pass.hpp"
        TARGET_FILES            ${ARGS_TARGET_FILES}
        PARSE_INCLUDED_MARKED   ON
        EXTRA_COMPILE_OPTIONS   -D__LUX_PARSE_TIME__=1 ${ARGS_EXTRA_COMPILE_OPTIONS}
    )
    add_meta(
        NAME                    ${ARGS_NAME}_pass_glslh
        MARKER                  "luxpass"
        TEMPLATE                ${_glslh_template}
        OUT_DIR                 ${_out_dir}
        META_SUFFIX             ".lglslh"
        TARGET_FILES            ${ARGS_TARGET_FILES}
        PARSE_INCLUDED_MARKED   ON
        EXTRA_COMPILE_OPTIONS   -D__LUX_PARSE_TIME__=1 ${ARGS_EXTRA_COMPILE_OPTIONS}
    )

    _engine_target_add_meta_content_stable(NAME ${ARGS_NAME}_pass_hpp   TARGET ${ARGS_TARGET})
    _engine_target_add_meta_content_stable(NAME ${ARGS_NAME}_pass_glslh TARGET ${ARGS_TARGET})
    target_include_directories(${ARGS_TARGET} PRIVATE ${_out_dir})

    if(ARGS_OUT_DIR_VAR)
        set(${ARGS_OUT_DIR_VAR} ${_out_dir} PARENT_SCOPE)
    endif()
    message(STATUS "[pass-params] ${ARGS_TARGET} <- ${ARGS_NAME} (${_out_dir})")
endfunction()

# =============================================================================
# engine_add_comm_ops
#   Generates a feature's whole comm/Operation face from ONE annotated header
#   (marker family `luxop` — see MetaAnnotations.hpp):
#     <Stem>.ops.hpp  op descriptors + CommandTraits + OpIds + payload-taking
#                     Proxy declaration (installed client surface)
#     <Stem>.client.ops.cpp  proxy/client bodies (compiled into CLIENT_TARGET)
#     <Stem>.ops.cpp         createFn/registrar/factory (compiled into
#                            IMPLEMENTATION_TARGET; declares the hand-written
#                            handle<OpName> contracts)
#   Outputs land under comm_gen/<INCLUDE_PREFIX>/ so the generated public include
#   spelling is identical
#   in-tree (BUILD_INTERFACE root) and installed (FILES_MATCHING *.ops.hpp).
#
#   Parameters:
#     NAME / INCLUDE_PREFIX / TARGET_FILES / EXTRA_COMPILE_OPTIONS — as usual.
#     CLIENT_TARGET and IMPLEMENTATION_TARGET may be supplied together or in
#     separate calls.  This lets the backend-neutral client own its generated
#     public surface even when a build profile intentionally omits the backend
#     implementation target.  An implementation-only call must already obtain
#     the generated public include directory through its client dependency.
#     One add_meta output is created per requested face and header (the cpp
#     templates need a per-file `stem`).
# =============================================================================
function(engine_add_comm_ops)
    set(one_value_args NAME CLIENT_TARGET IMPLEMENTATION_TARGET INCLUDE_PREFIX)
    set(multi_value_args TARGET_FILES EXTRA_COMPILE_OPTIONS)
    cmake_parse_arguments(ARGS "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT ARGS_NAME OR NOT ARGS_INCLUDE_PREFIX OR NOT ARGS_TARGET_FILES OR
       (NOT ARGS_CLIENT_TARGET AND NOT ARGS_IMPLEMENTATION_TARGET))
        message(FATAL_ERROR
            "[engine_add_comm_ops] NAME, INCLUDE_PREFIX, TARGET_FILES and at "
            "least one of CLIENT_TARGET / IMPLEMENTATION_TARGET are required"
        )
    endif()

    if(ARGS_CLIENT_TARGET)
        if(NOT TARGET ${ARGS_CLIENT_TARGET})
            message(FATAL_ERROR
                "[engine_add_comm_ops] CLIENT_TARGET "
                "'${ARGS_CLIENT_TARGET}' does not exist"
            )
        endif()
    endif()
    if(ARGS_IMPLEMENTATION_TARGET)
        if(NOT TARGET ${ARGS_IMPLEMENTATION_TARGET})
            message(FATAL_ERROR
                "[engine_add_comm_ops] IMPLEMENTATION_TARGET "
                "'${ARGS_IMPLEMENTATION_TARGET}' does not exist"
            )
        endif()
    endif()

    if(IS_ABSOLUTE "${ARGS_INCLUDE_PREFIX}" OR ARGS_INCLUDE_PREFIX MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR
            "[engine_add_comm_ops] INCLUDE_PREFIX must be an install-relative path"
        )
    endif()

    get_filename_component(LUX_ENGINE_META_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR} DIRECTORY)
    set(_hpp_template "${LUX_ENGINE_META_DIR}/template/comm_ops_hpp.template")
    set(_client_cpp_template "${LUX_ENGINE_META_DIR}/template/comm_ops_client_cpp.template")
    set(_cpp_template "${LUX_ENGINE_META_DIR}/template/comm_ops_cpp.template")
    set(_root    "${CMAKE_CURRENT_BINARY_DIR}/comm_gen")
    set(_out_dir "${_root}/${ARGS_INCLUDE_PREFIX}")
    file(MAKE_DIRECTORY "${_out_dir}")

    foreach(_header IN LISTS ARGS_TARGET_FILES)
        get_filename_component(_stem "${_header}" NAME_WE)
        if(ARGS_CLIENT_TARGET)
            add_meta(
                NAME                    ${ARGS_NAME}_${_stem}_ops_hpp
                MARKER                  "luxop"
                TEMPLATE                ${_hpp_template}
                OUT_DIR                 ${_out_dir}
                META_SUFFIX             ".ops.hpp"
                TARGET_FILES            ${_header}
                JSON_FIELD              "{\"stem\":\"${_stem}\"}"
                EXTRA_COMPILE_OPTIONS   -D__LUX_PARSE_TIME__=1 ${ARGS_EXTRA_COMPILE_OPTIONS}
            )
            add_meta(
                NAME                    ${ARGS_NAME}_${_stem}_client_ops_cpp
                MARKER                  "luxop"
                TEMPLATE                ${_client_cpp_template}
                OUT_DIR                 ${_out_dir}
                META_SUFFIX             ".client.ops.cpp"
                TARGET_FILES            ${_header}
                JSON_FIELD              "{\"stem\":\"${_stem}\"}"
                EXTRA_COMPILE_OPTIONS   -D__LUX_PARSE_TIME__=1 ${ARGS_EXTRA_COMPILE_OPTIONS}
            )
            _engine_target_add_meta_content_stable(
                NAME ${ARGS_NAME}_${_stem}_ops_hpp
                TARGET ${ARGS_CLIENT_TARGET}
            )
            _engine_target_add_meta_content_stable(
                NAME ${ARGS_NAME}_${_stem}_client_ops_cpp
                TARGET ${ARGS_CLIENT_TARGET}
            )
        endif()

        if(ARGS_IMPLEMENTATION_TARGET)
            add_meta(
                NAME                    ${ARGS_NAME}_${_stem}_ops_cpp
                MARKER                  "luxop"
                TEMPLATE                ${_cpp_template}
                OUT_DIR                 ${_out_dir}
                META_SUFFIX             ".ops.cpp"
                TARGET_FILES            ${_header}
                JSON_FIELD              "{\"stem\":\"${_stem}\",\"include_prefix\":\"${ARGS_INCLUDE_PREFIX}\"}"
                EXTRA_COMPILE_OPTIONS   -D__LUX_PARSE_TIME__=1 ${ARGS_EXTRA_COMPILE_OPTIONS}
            )
            _engine_target_add_meta_content_stable(
                NAME ${ARGS_NAME}_${_stem}_ops_cpp
                TARGET ${ARGS_IMPLEMENTATION_TARGET}
            )
        endif()
    endforeach()

    if(ARGS_CLIENT_TARGET)
        target_include_directories(
            ${ARGS_CLIENT_TARGET}
            PUBLIC "$<BUILD_INTERFACE:${_root}>"
        )
        install(DIRECTORY "${_root}/" DESTINATION include
                FILES_MATCHING PATTERN "*.ops.hpp")
        message(STATUS
            "[comm-ops] ${ARGS_CLIENT_TARGET}:client <- ${ARGS_NAME} (${_out_dir})"
        )
    endif()

    if(ARGS_IMPLEMENTATION_TARGET)
        target_include_directories(
            ${ARGS_IMPLEMENTATION_TARGET}
            PRIVATE "$<BUILD_INTERFACE:${_root}>"
        )
        message(STATUS
            "[comm-ops] ${ARGS_IMPLEMENTATION_TARGET}:implementation "
            "<- ${ARGS_NAME} (${_out_dir})"
        )
    endif()
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

# 宿主导出生成物的位置(相对 install 前缀)。交叉侧沿 CMAKE_PREFIX_PATH 找它。
#
# 必须是 CACHE INTERNAL 而不是普通 set:本文件由 include_component_cmake_scripts()
# 引入,而那是个 **function**,普通 set 会被困在它的作用域里,到了下面的函数体里
# 就是空字符串 —— 表现为 install 的 DESTINATION 变成 "/<slug>",既不报错也装不到
# 想去的地方。踩过一次,记在这里。
set(LUX_META_SHARED_RELDIR "share/lux-engine/meta_gen"
    CACHE INTERNAL "Install-prefix-relative location of exported generated meta sources")

# 找到某个 slug 的宿主生成物目录;找不到时返回空。
function(_lux_meta_shared_dir out_var slug)
    set(${out_var} "" PARENT_SCOPE)
    # 允许显式指定,便于把生成物放在非 install 的地方(CI、缓存目录)
    if(LUX_HOST_META_DIR AND IS_DIRECTORY "${LUX_HOST_META_DIR}/${slug}")
        set(${out_var} "${LUX_HOST_META_DIR}/${slug}" PARENT_SCOPE)
        return()
    endif()
    if(CMAKE_CROSSCOMPILING)
        return()
    endif()
    foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
        set(_cand "${_prefix}/${LUX_META_SHARED_RELDIR}/${slug}")
        if(IS_DIRECTORY "${_cand}")
            set(${out_var} "${_cand}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# 输入头的内容指纹。用内容而非时间戳:跨机器/跨检出时间戳没有可比性,而内容有。
function(_lux_meta_fingerprint out_var headers)
    set(_acc "")
    foreach(_h IN LISTS headers)
        if(EXISTS "${_h}")
            file(SHA256 "${_h}" _one)
            get_filename_component(_name "${_h}" NAME)
            string(APPEND _acc "${_name}:${_one}\n")
        endif()
    endforeach()
    string(SHA256 _fp "${_acc}")
    set(${out_var} "${_fp}" PARENT_SCOPE)
endfunction()

# 宿主生成物是否与当前这批头匹配。
function(_lux_meta_stamp_matches out_var shared_dir headers)
    set(${out_var} FALSE PARENT_SCOPE)
    set(_stamp "${shared_dir}/inputs.sha256")
    if(NOT EXISTS "${_stamp}")
        return()
    endif()
    file(READ "${_stamp}" _recorded)
    string(STRIP "${_recorded}" _recorded)
    _lux_meta_fingerprint(_current "${headers}")
    if(_recorded STREQUAL _current)
        set(${out_var} TRUE PARENT_SCOPE)
    endif()
endfunction()

# 消费宿主生成物:把 .cpp 挂上 owner,并就地拼出聚合头。
#
# 聚合头不需要生成器 —— engine_target_add_meta 里那段本来就是 configure_file
# 从函数名拼的,而函数名由头文件名推导,是确定的。这里复用同一套推导。
function(_lux_meta_use_prebuilt)
    set(one_value_args OWNER LUA_OWNER SLUG SHARED_DIR OUT_DIR META_OUT_HPP LUA_OUT_HPP META_PREFIX LUA_BINDING)
    set(multi_value_args TARGET_FILES)
    cmake_parse_arguments(P "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    set(_meta_sources "")
    set(_lua_sources "")
    set(_func_names "")
    foreach(_header IN LISTS P_TARGET_FILES)
        get_filename_component(_stem "${_header}" NAME_WE)
        string(REGEX REPLACE "([A-Z])" "_\\1" _snake "${_stem}")
        string(TOLOWER "${_snake}" _snake)
        string(REGEX REPLACE "^_" "" _snake "${_snake}")
        list(APPEND _func_names "${P_SLUG}_${_snake}")

        set(_cpp "${P_SHARED_DIR}/${_stem}.meta.cpp")
        if(NOT EXISTS "${_cpp}")
            message(FATAL_ERROR
                "[meta] ${P_SLUG}: 指纹匹配但缺少 ${_cpp} —— 宿主导出不完整。"
                "重新构建并安装宿主侧。")
        endif()
        list(APPEND _meta_sources "${_cpp}")

        if(P_LUA_BINDING)
            set(_lua "${P_SHARED_DIR}/${_stem}.lua.cpp")
            if(EXISTS "${_lua}")
                list(APPEND _lua_sources "${_lua}")
            endif()
        endif()
    endforeach()

    target_sources(${P_OWNER} PRIVATE ${_meta_sources})

    # 聚合头(与 engine_target_add_meta 末尾同形)
    set(_externs "")
    set(_calls   "")
    foreach(f IN LISTS _func_names)
        set(_externs "${_externs}\nvoid ${f}_meta(lux::meta::ReflectionRegistry&, lux::meta::qual_type_index_fix_list&);")
        set(_calls   "${_calls}\n    ${f}_meta(registry, fix_list);")
    endforeach()
    set(EXTERN_PROTOTYPES    "${_externs}")
    set(META_CALL_STATEMENTS "${_calls}")
    set(REGISTER_FUNC_PREFIX "${P_META_PREFIX}")

    get_filename_component(LUX_ENGINE_META_DIR ${CMAKE_CURRENT_FUNCTION_LIST_DIR} DIRECTORY)
    configure_file(
        "${LUX_ENGINE_META_DIR}/template/meta_registration.hpp.in"
        "${P_OUT_DIR}/${P_META_OUT_HPP}"
        @ONLY)

    target_include_directories(${P_OWNER} PUBLIC $<BUILD_INTERFACE:${P_OUT_DIR}>)

    if(P_LUA_BINDING)
        if(NOT P_LUA_OWNER)
            set(P_LUA_OWNER ${P_OWNER})
        endif()
        if(_lua_sources)
            target_sources(${P_LUA_OWNER} PRIVATE ${_lua_sources})
        endif()

        set(_lua_externs "")
        set(_lua_calls "")
        foreach(f IN LISTS _func_names)
            set(_lua_externs
                "${_lua_externs}\nvoid ${f}_lua(sol::state_view&);")
            set(_lua_calls "${_lua_calls}\n    ${f}_lua(state);")
        endforeach()
        set(EXTERN_PROTOTYPES   "${_lua_externs}")
        set(SOL_CALL_STATEMENTS "${_lua_calls}")
        set(REGISTER_FUNC_PREFIX "${P_META_PREFIX}")
        configure_file(
            "${LUX_ENGINE_META_DIR}/template/lua_registration.hpp.in"
            "${P_OUT_DIR}/${P_LUA_OUT_HPP}"
            @ONLY)
        target_include_directories(
            ${P_LUA_OWNER}
            PUBLIC $<BUILD_INTERFACE:${P_OUT_DIR}>)
    endif()
    message(STATUS "[meta] ${P_OWNER} <- 宿主生成物 (${P_SLUG})")
endfunction()

function(engine_enable_module_meta)
    set(options       LUA_BINDING)
    set(one_value_args TARGET SIDECAR_TARGET LUA_SIDECAR_TARGET)
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

    if(ARGS_LUA_SIDECAR_TARGET)
        if(NOT ARGS_LUA_BINDING)
            message(FATAL_ERROR
                "[engine_enable_module_meta] LUA_SIDECAR_TARGET requires LUA_BINDING")
        endif()
        if(NOT TARGET ${ARGS_LUA_SIDECAR_TARGET})
            message(FATAL_ERROR
                "[engine_enable_module_meta] LUA_SIDECAR_TARGET "
                "'${ARGS_LUA_SIDECAR_TARGET}' must be created by the caller")
        endif()
        set(_lua_owner_target ${ARGS_LUA_SIDECAR_TARGET})
    else()
        set(_lua_owner_target ${_meta_owner_target})
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

    # ------------------------------------------------------------------------
    # 生成一次,各平台共用
    # ------------------------------------------------------------------------
    # 生成物是 ABI 无关的可移植 C++(字段偏移走 offsetof,由目标编译器求值),
    # 实测两平台逐字节相同。所以它没有理由每个平台各生成一遍 —— 交叉构建直接
    # 消费宿主导出的那份。
    #
    # 这不是新增依赖:交叉构建**本来就**硬依赖宿主 install(生成器 exe 就是从
    # 那里 find_program 出来的)。这里只是让同一条依赖多带一样东西。
    #
    # 陈旧防护:导出时把输入头的内容指纹写进 .stamp,交叉侧比对不上就**回退到
    # 本地生成**而不是默默用旧的。宁可多生成一次,也不要让 Android 用着上一版
    # 的反射跑。
    _lux_meta_shared_dir(_shared_dir "${_slug}")
    set(_reuse_shared FALSE)
    if(CMAKE_CROSSCOMPILING AND _shared_dir)
        _lux_meta_stamp_matches(_reuse_shared "${_shared_dir}" "${ARGS_TARGET_FILES}")
        if(_reuse_shared)
            message(STATUS "[meta] ${_slug}: 复用宿主生成物 ${_shared_dir}")
        else()
            message(STATUS "[meta] ${_slug}: 宿主生成物缺失或已陈旧,本地重新生成")
        endif()
    endif()

    if(_reuse_shared)
        _lux_meta_use_prebuilt(
            OWNER           ${_meta_owner_target}
            LUA_OWNER       ${_lua_owner_target}
            SLUG            ${_slug}
            SHARED_DIR      "${_shared_dir}"
            OUT_DIR         "${_out_dir}"
            META_OUT_HPP    "${_meta_out_hpp}"
            LUA_OUT_HPP     "${_lua_out_hpp}"
            META_PREFIX     "${_meta_prefix}"
            TARGET_FILES    ${ARGS_TARGET_FILES}
            LUA_BINDING     ${ARGS_LUA_BINDING}
        )
        return()
    endif()

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

    # --- 宿主侧导出生成物,供交叉构建复用(见函数开头的"生成一次"说明) -----
    # 只在非交叉时导出:交叉树里的产物没有资格当共享源,它自己就是消费者。
    if(NOT CMAKE_CROSSCOMPILING)
        _lux_meta_fingerprint(_fp "${ARGS_TARGET_FILES}")
        set(_stamp_file "${CMAKE_CURRENT_BINARY_DIR}/meta_gen/inputs.sha256")
        file(WRITE "${_stamp_file}" "${_fp}")

        install(
            DIRECTORY   "${CMAKE_CURRENT_BINARY_DIR}/meta_gen/"
            DESTINATION "${LUX_META_SHARED_RELDIR}/${_slug}"
            FILES_MATCHING
                PATTERN "*.meta.cpp"
                PATTERN "*.lua.cpp"
                PATTERN "inputs.sha256"
        )
    endif()

    if(ARGS_LUA_BINDING AND _lua_targets)
        engine_target_add_lua_binding(
            TARGET                         ${_lua_owner_target}
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
    if(ARGS_LUA_BINDING AND
       NOT _lua_owner_target STREQUAL _meta_owner_target)
        target_include_directories(${_lua_owner_target}
            PUBLIC "$<BUILD_INTERFACE:${_out_dir}>")
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
