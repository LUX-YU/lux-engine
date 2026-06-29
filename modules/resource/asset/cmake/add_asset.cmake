# ===========================================
# Include guard
# ===========================================
if(_COMPONENT_ASSET_TOOLS_INCLUDED_)
  return()
endif()
set(_COMPONENT_ASSET_TOOLS_INCLUDED_ TRUE)

# -------------------------------------------------
# add_asset
#   定义资产打包对象
#   必选: NAME OUT_DIR
#   可选: PACKER TYPE INSPECT (ON/OFF)
#   复数: ENTRIES "type|src|relout" ...
#   开关: ECHO ALWAYS_REGENERATE
function(add_asset)
    set(one_value_args
        NAME
        PACKER
        OUT_DIR
        TYPE
        INSPECT
    )
    set(multi_value_args
        ENTRIES
    )
    set(optional_args ECHO ALWAYS_REGENERATE)
    cmake_parse_arguments(ASSET "${optional_args}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT ASSET_NAME)
        message(FATAL_ERROR "[add_asset] NAME parameter is required")
    endif()
    set(_asset_name "${ASSET_NAME}")

    if(NOT ASSET_OUT_DIR)
        set(ASSET_OUT_DIR "${CMAKE_BINARY_DIR}/assets")
    endif()
    if(NOT ASSET_INSPECT)
        set(ASSET_INSPECT OFF)
    endif()

    # 选择 packer：优先同工程 target，其次系统路径
    set(LUX_ASSET_PACKER_TARGET "")
    if(NOT ASSET_PACKER)
        if(TARGET lux_asset_packer)
            set(LUX_ASSET_PACKER "$<TARGET_FILE:lux_asset_packer>")
            set(LUX_ASSET_PACKER_TARGET "lux_asset_packer")
        else()
            find_program(LUX_ASSET_PACKER lux_asset_packer REQUIRED)
            if(NOT LUX_ASSET_PACKER)
                message(FATAL_ERROR "[add_asset] Could not find 'lux_asset_packer' executable")
            endif()
        endif()
    else()
        set(LUX_ASSET_PACKER "${ASSET_PACKER}")
        set(LUX_ASSET_PACKER_TARGET "")
    endif()

    # 占位 target
    if(NOT TARGET ${_asset_name})
        add_custom_target(${_asset_name} ALL
            COMMENT "[add_asset] Placeholder target for asset object '${_asset_name}'."
        )
    endif()

    # 多条 entry 用换行拼接保存；每条形如 "type|src|relout"
    if(ASSET_ENTRIES)
        string(JOIN "\n" _entries_joined ${ASSET_ENTRIES})
    else()
        set(_entries_joined "")
    endif()

    set_target_properties(${_asset_name} PROPERTIES
        ASSET_PACKER           "${LUX_ASSET_PACKER}"
        ASSET_PACKER_TARGET    "${LUX_ASSET_PACKER_TARGET}"
        ASSET_OUT_DIR          "${ASSET_OUT_DIR}"
        ASSET_DEFAULT_TYPE     "${ASSET_TYPE}"
        ASSET_ENTRIES_RAW      "${_entries_joined}"     # 换行分隔的原始字符串
        ASSET_ECHO             "${ASSET_ECHO}"
        ASSET_ALWAYS_REGEN     "${ASSET_ALWAYS_REGENERATE}"
        ASSET_INSPECT          "${ASSET_INSPECT}"
    )
endfunction()

# -------------------------------------------------
# _asset_normalize_texture_entries
#   规范化纹理条目为: "texture|src|relout"
#   支持输入: "src" / "src|relout" / "texture|src|relout"
function(_asset_normalize_texture_entries input_var out_var caller_name)
    set(_normalized "")
    foreach(_entry IN LISTS ${input_var})
        string(REPLACE "|" ";" _fields "${_entry}")
        list(LENGTH _fields _flen)

        if(_flen EQUAL 1)
            list(GET _fields 0 _src)
            get_filename_component(_base "${_src}" NAME_WE)
            set(_relout "${_base}.luxasset")
        elseif(_flen EQUAL 2)
            list(GET _fields 0 _src)
            list(GET _fields 1 _relout)
        elseif(_flen EQUAL 3)
            list(GET _fields 0 _etype)
            if(NOT _etype STREQUAL "texture")
                message(FATAL_ERROR "[${caller_name}] Invalid texture entry '${_entry}', expected 'src|relout' or 'texture|src|relout'")
            endif()
            list(GET _fields 1 _src)
            list(GET _fields 2 _relout)
        else()
            message(FATAL_ERROR "[${caller_name}] Invalid texture entry '${_entry}', expected 'src|relout' or 'texture|src|relout'")
        endif()

        if(NOT _src)
            message(FATAL_ERROR "[${caller_name}] Invalid texture entry '${_entry}', source path is empty")
        endif()

        list(APPEND _normalized "texture|${_src}|${_relout}")
    endforeach()

    set(${out_var} ${_normalized} PARENT_SCOPE)
endfunction()

# -------------------------------------------------
# add_texture_asset
#   纹理资产专用定义
#   ENTRIES 支持: "src" / "src|relout" / "texture|src|relout"
#   可选: TEXTURE_FORMAT TEXTURE_COLOR_SPACE
function(add_texture_asset)
    set(one_value_args
        NAME
        PACKER
        OUT_DIR
        INSPECT
        TEXTURE_FORMAT
        TEXTURE_COLOR_SPACE
    )
    set(multi_value_args
        ENTRIES
    )
    set(optional_args ECHO ALWAYS_REGENERATE)
    cmake_parse_arguments(TEX "${optional_args}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT TEX_NAME)
        message(FATAL_ERROR "[add_texture_asset] NAME parameter is required")
    endif()

    set(_normalized_entries "")
    if(TEX_ENTRIES)
        _asset_normalize_texture_entries(TEX_ENTRIES _normalized_entries add_texture_asset)
    endif()

    set(_add_args
        NAME "${TEX_NAME}"
        TYPE "texture"
    )
    if(TEX_PACKER)
        list(APPEND _add_args PACKER "${TEX_PACKER}")
    endif()
    if(TEX_OUT_DIR)
        list(APPEND _add_args OUT_DIR "${TEX_OUT_DIR}")
    endif()
    if(TEX_INSPECT)
        list(APPEND _add_args INSPECT "${TEX_INSPECT}")
    endif()
    if(_normalized_entries)
        list(APPEND _add_args ENTRIES ${_normalized_entries})
    endif()
    if(TEX_ECHO)
        list(APPEND _add_args ECHO)
    endif()
    if(TEX_ALWAYS_REGENERATE)
        list(APPEND _add_args ALWAYS_REGENERATE)
    endif()

    add_asset(${_add_args})

    if(TEX_TEXTURE_FORMAT)
        set_target_properties(${TEX_NAME} PROPERTIES
            ASSET_TEXTURE_FORMAT "${TEX_TEXTURE_FORMAT}"
        )
    endif()
    if(TEX_TEXTURE_COLOR_SPACE)
        set_target_properties(${TEX_NAME} PROPERTIES
            ASSET_TEXTURE_COLOR_SPACE "${TEX_TEXTURE_COLOR_SPACE}"
        )
    endif()
endfunction()

# -------------------------------------------------
# asset_add_files
#   追加条目：ENTRIES "type|src|relout" ...
function(asset_add_files)
    if("${ARGV0}" STREQUAL "")
        message(FATAL_ERROR "[asset_add_files] The first argument must be the AssetObjectName")
    endif()
    set(_asset_name "${ARGV0}")
    list(REMOVE_AT ARGV 0)

    set(multi_value_args ENTRIES)
    cmake_parse_arguments(A "" "" "${multi_value_args}" ${ARGV})
    if(NOT A_ENTRIES)
        message(FATAL_ERROR "[asset_add_files] ENTRIES is required")
    endif()

    get_target_property(_entries_raw ${_asset_name} ASSET_ENTRIES_RAW)
    set(_entries_list "")
    if(_entries_raw)
        string(REPLACE "\r\n" "\n" _entries_raw_norm "${_entries_raw}")
        string(REPLACE "\r"   "\n" _entries_raw_norm "${_entries_raw_norm}")
        string(REGEX MATCHALL "[^\n]+" _entries_list "${_entries_raw_norm}")
    endif()

    list(APPEND _entries_list ${A_ENTRIES})
    list(REMOVE_DUPLICATES _entries_list)

    if(_entries_list)
        string(JOIN "\n" _joined "${_entries_list}")
    else()
        set(_joined "")
    endif()

    set_target_properties(${_asset_name} PROPERTIES
        ASSET_ENTRIES_RAW "${_joined}"
    )
endfunction()

# -------------------------------------------------
# asset_add_textures
#   追加纹理条目：ENTRIES "src|relout" ...
#   可选更新纹理默认参数：TEXTURE_FORMAT / TEXTURE_COLOR_SPACE
function(asset_add_textures)
    if("${ARGV0}" STREQUAL "")
        message(FATAL_ERROR "[asset_add_textures] The first argument must be the AssetObjectName")
    endif()
    set(_asset_name "${ARGV0}")
    list(REMOVE_AT ARGV 0)

    set(one_value_args
        TEXTURE_FORMAT
        TEXTURE_COLOR_SPACE
    )
    set(multi_value_args ENTRIES)
    cmake_parse_arguments(TEXADD "" "${one_value_args}" "${multi_value_args}" ${ARGV})
    if(NOT TEXADD_ENTRIES)
        message(FATAL_ERROR "[asset_add_textures] ENTRIES is required")
    endif()

    _asset_normalize_texture_entries(TEXADD_ENTRIES _normalized_entries asset_add_textures)
    asset_add_files(${_asset_name} ENTRIES ${_normalized_entries})

    if(TEXADD_TEXTURE_FORMAT)
        set_target_properties(${_asset_name} PROPERTIES
            ASSET_TEXTURE_FORMAT "${TEXADD_TEXTURE_FORMAT}"
        )
    endif()
    if(TEXADD_TEXTURE_COLOR_SPACE)
        set_target_properties(${_asset_name} PROPERTIES
            ASSET_TEXTURE_COLOR_SPACE "${TEXADD_TEXTURE_COLOR_SPACE}"
        )
    endif()
endfunction()

# -------------------------------------------------
# target_add_asset
#   将资产构建接入某个目标：
#     - 每条 "type|src|relout" 生成 .luxasset
#     - 聚合为 <NAME>_gen
#     - <TARGET> 依赖 <NAME>_gen
#     - 若 packer 为 in-tree target，确保先构建
function(target_add_asset)
    set(one_value_args NAME TARGET)
    set(optional_args ECHO ALWAYS_REGENERATE)
    cmake_parse_arguments(ARGS "${optional_args}" "${one_value_args}" "" ${ARGN})

    if(NOT ARGS_NAME)
        message(FATAL_ERROR "[target_add_asset] NAME parameter is required")
    endif()
    if(NOT ARGS_TARGET)
        message(FATAL_ERROR "[target_add_asset] TARGET parameter is required")
    endif()

    set(_asset_name  "${ARGS_NAME}")
    set(_target_name "${ARGS_TARGET}")

    # 读取配置
    get_target_property(_packer_path   ${_asset_name} ASSET_PACKER)
    get_target_property(_packer_target ${_asset_name} ASSET_PACKER_TARGET)
    get_target_property(_out_dir       ${_asset_name} ASSET_OUT_DIR)
    get_target_property(_entries_raw   ${_asset_name} ASSET_ENTRIES_RAW)
    get_target_property(_def_type      ${_asset_name} ASSET_DEFAULT_TYPE)
    get_target_property(_echo          ${_asset_name} ASSET_ECHO)
    get_target_property(_always        ${_asset_name} ASSET_ALWAYS_REGEN)
    get_target_property(_inspect       ${_asset_name} ASSET_INSPECT)
    get_target_property(_texture_format ${_asset_name} ASSET_TEXTURE_FORMAT)
    get_target_property(_texture_color_space ${_asset_name} ASSET_TEXTURE_COLOR_SPACE)

    if(NOT _packer_path)
        message(FATAL_ERROR "[target_add_asset] Missing ASSET_PACKER on ${_asset_name}")
    endif()
    if(NOT _out_dir)
        set(_out_dir "${CMAKE_BINARY_DIR}/assets")
    endif()
    if(NOT _entries_raw)
        message(FATAL_ERROR "[target_add_asset] The ASSET_ENTRIES for '${_asset_name}' is empty.")
    endif()

    if(NOT EXISTS "${_out_dir}")
        file(MAKE_DIRECTORY "${_out_dir}")
    endif()

    # 按行提取
    string(REPLACE "\r\n" "\n" _entries_raw_norm "${_entries_raw}")
    string(REPLACE "\r"   "\n" _entries_raw_norm "${_entries_raw_norm}")
    string(REGEX MATCHALL "[^\n]+" _entries_lines "${_entries_raw_norm}")

    # 逐条目生成命令（用 '|' 作为行内分隔符）
    set(_all_outputs "")
    list(LENGTH _entries_lines _elen_total)
    math(EXPR _last_idx "${_elen_total}-1")

    foreach(i RANGE 0 ${_last_idx})
        list(GET _entries_lines ${i} entry_line)

        # "type|src|relout" -> 列表
        string(REPLACE "|" ";" entry_fields "${entry_line}")
        list(LENGTH entry_fields _elen)
        if(_elen LESS 2)
            message(FATAL_ERROR "[target_add_asset] Invalid entry '${entry_line}', expected 'type|src|relout'")
        endif()

        list(GET entry_fields 0 _etype)
        list(GET entry_fields 1 _src)
        if(_elen GREATER 2)
            list(GET entry_fields 2 _relout)
        else()
            get_filename_component(_base "${_src}" NAME_WE)
            set(_relout "${_base}.luxasset")
        endif()

        if(NOT _etype OR _etype STREQUAL "")
            if(NOT _def_type)
                message(FATAL_ERROR "[target_add_asset] Entry has no type and no default TYPE is set: '${entry_line}'")
            endif()
            set(_etype "${_def_type}")
        endif()

        set(_out "${_out_dir}/${_relout}")
        get_filename_component(_parent "${_out}" DIRECTORY)
        if(NOT EXISTS "${_parent}")
            file(MAKE_DIRECTORY "${_parent}")
        endif()

        set(_extra_texture_args "")
        if(_etype STREQUAL "texture")
            if(_texture_format)
                list(APPEND _extra_texture_args --texture_format ${_texture_format})
            endif()
            if(_texture_color_space)
                list(APPEND _extra_texture_args --texture_color_space ${_texture_color_space})
            endif()
        endif()

        if(_inspect)
            add_custom_command(
                OUTPUT "${_out}"
                COMMAND ${_packer_path} --type ${_etype} --source_path "${_src}" --target_path "${_out}" ${_extra_texture_args}
                COMMAND ${_packer_path} --inspect --target_path "${_out}"
                DEPENDS "${_src}"
                COMMENT "[target_add_asset] Pack '${entry_line}' -> ${_out}"
                VERBATIM
            )
        else()
            add_custom_command(
                OUTPUT "${_out}"
                COMMAND ${_packer_path} --type ${_etype} --source_path "${_src}" --target_path "${_out}" ${_extra_texture_args}
                DEPENDS "${_src}"
                COMMENT "[target_add_asset] Pack '${entry_line}' -> ${_out}"
                VERBATIM
            )
        endif()

        list(APPEND _all_outputs "${_out}")
    endforeach()

    # 聚合生成目标
    set(_asset_gen_target "${_asset_name}_gen")
    add_custom_target("${_asset_gen_target}"
        DEPENDS ${_all_outputs}
    )

    # 确保先生成 packer（若来自 in-tree target）
    if(_packer_target)
        add_dependencies("${_asset_gen_target}" "${_packer_target}")
    endif()

    # 用户 target 依赖资产
    add_dependencies("${_target_name}" "${_asset_gen_target}")

    # ALWAYS_REGENERATE：可选，标记跳过缓存
    if(ARGS_ALWAYS_REGENERATE OR _always)
        foreach(_of IN LISTS _all_outputs)
            set_property(SOURCE "${_of}" PROPERTY SKIP_CACHE TRUE)
        endforeach()
    endif()
endfunction()

