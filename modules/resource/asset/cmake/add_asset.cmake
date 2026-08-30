include_guard(GLOBAL)

# Build-tool-only cooked asset packing. Runtime targets consume only the
# generated .luxasset files; the packer executable is never linked.
function(add_asset)
    set(one_value_args NAME PACKER OUT_DIR TYPE INSPECT)
    set(multi_value_args ENTRIES)
    set(optional_args ECHO ALWAYS_REGENERATE)
    cmake_parse_arguments(ASSET "${optional_args}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT ASSET_NAME)
        message(FATAL_ERROR "[add_asset] NAME is required")
    endif()
    if(NOT ASSET_OUT_DIR)
        set(ASSET_OUT_DIR "${CMAKE_BINARY_DIR}/assets")
    endif()

    set(packer_target "")
    if(ASSET_PACKER)
        set(packer "${ASSET_PACKER}")
    elseif(LUX_ASSET_PACKER_EXECUTABLE)
        if(NOT EXISTS "${LUX_ASSET_PACKER_EXECUTABLE}")
            message(FATAL_ERROR
                "[add_asset] LUX_ASSET_PACKER_EXECUTABLE does not exist: ${LUX_ASSET_PACKER_EXECUTABLE}"
            )
        endif()
        set(packer "${LUX_ASSET_PACKER_EXECUTABLE}")
    elseif(TARGET lux_asset_packer)
        set(packer "$<TARGET_FILE:lux_asset_packer>")
        set(packer_target lux_asset_packer)
    elseif(COMMAND lux_find_host_program)
        lux_find_host_program(packer REQUIRED NAMES lux_asset_packer)
    elseif(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "[add_asset] cross builds require lux_find_host_program")
    else()
        find_program(packer lux_asset_packer REQUIRED)
    endif()

    if(NOT TARGET ${ASSET_NAME})
        add_custom_target(${ASSET_NAME})
    endif()
    if(ASSET_ENTRIES)
        string(JOIN "\n" entries ${ASSET_ENTRIES})
    else()
        set(entries "")
    endif()
    set_target_properties(
        ${ASSET_NAME}
        PROPERTIES
            LUX_ASSET_PACKER "${packer}"
            LUX_ASSET_PACKER_TARGET "${packer_target}"
            LUX_ASSET_OUT_DIR "${ASSET_OUT_DIR}"
            LUX_ASSET_DEFAULT_TYPE "${ASSET_TYPE}"
            LUX_ASSET_ENTRIES "${entries}"
            LUX_ASSET_INSPECT "${ASSET_INSPECT}"
            LUX_ASSET_ALWAYS_REGENERATE "${ASSET_ALWAYS_REGENERATE}"
    )
endfunction()

function(add_texture_asset)
    set(one_value_args NAME PACKER OUT_DIR INSPECT TEXTURE_FORMAT TEXTURE_COLOR_SPACE)
    set(multi_value_args ENTRIES)
    set(optional_args ECHO ALWAYS_REGENERATE)
    cmake_parse_arguments(TEXTURE "${optional_args}" "${one_value_args}" "${multi_value_args}" ${ARGN})
    if(NOT TEXTURE_NAME)
        message(FATAL_ERROR "[add_texture_asset] NAME is required")
    endif()

    set(entries)
    foreach(entry IN LISTS TEXTURE_ENTRIES)
        string(REPLACE "|" ";" fields "${entry}")
        list(LENGTH fields field_count)
        if(field_count EQUAL 1)
            list(GET fields 0 source)
            get_filename_component(base "${source}" NAME_WE)
            list(APPEND entries "texture|${source}|${base}.luxasset")
        elseif(field_count EQUAL 2)
            list(GET fields 0 source)
            list(GET fields 1 output)
            list(APPEND entries "texture|${source}|${output}")
        elseif(field_count EQUAL 3)
            list(GET fields 0 type)
            if(NOT type STREQUAL "texture")
                message(FATAL_ERROR "[add_texture_asset] invalid entry '${entry}'")
            endif()
            list(APPEND entries "${entry}")
        else()
            message(FATAL_ERROR "[add_texture_asset] invalid entry '${entry}'")
        endif()
    endforeach()

    set(arguments NAME "${TEXTURE_NAME}" TYPE texture ENTRIES ${entries})
    foreach(name IN ITEMS PACKER OUT_DIR INSPECT)
        if(TEXTURE_${name})
            list(APPEND arguments ${name} "${TEXTURE_${name}}")
        endif()
    endforeach()
    if(TEXTURE_ALWAYS_REGENERATE)
        list(APPEND arguments ALWAYS_REGENERATE)
    endif()
    add_asset(${arguments})
    set_target_properties(
        ${TEXTURE_NAME}
        PROPERTIES
            LUX_ASSET_TEXTURE_FORMAT "${TEXTURE_TEXTURE_FORMAT}"
            LUX_ASSET_TEXTURE_COLOR_SPACE "${TEXTURE_TEXTURE_COLOR_SPACE}"
    )
endfunction()

function(target_add_asset)
    set(one_value_args NAME TARGET)
    set(optional_args ECHO ALWAYS_REGENERATE)
    cmake_parse_arguments(ASSET "${optional_args}" "${one_value_args}" "" ${ARGN})
    if(NOT ASSET_NAME OR NOT ASSET_TARGET)
        message(FATAL_ERROR "[target_add_asset] NAME and TARGET are required")
    endif()

    get_target_property(packer ${ASSET_NAME} LUX_ASSET_PACKER)
    get_target_property(packer_target ${ASSET_NAME} LUX_ASSET_PACKER_TARGET)
    get_target_property(out_dir ${ASSET_NAME} LUX_ASSET_OUT_DIR)
    get_target_property(entries_raw ${ASSET_NAME} LUX_ASSET_ENTRIES)
    get_target_property(default_type ${ASSET_NAME} LUX_ASSET_DEFAULT_TYPE)
    get_target_property(inspect ${ASSET_NAME} LUX_ASSET_INSPECT)
    get_target_property(texture_format ${ASSET_NAME} LUX_ASSET_TEXTURE_FORMAT)
    get_target_property(texture_color_space ${ASSET_NAME} LUX_ASSET_TEXTURE_COLOR_SPACE)
    if(NOT packer OR NOT entries_raw)
        message(FATAL_ERROR "[target_add_asset] incomplete asset '${ASSET_NAME}'")
    endif()

    string(REPLACE "\r\n" "\n" entries_raw "${entries_raw}")
    string(REPLACE "\r" "\n" entries_raw "${entries_raw}")
    string(REGEX MATCHALL "[^\n]+" entries "${entries_raw}")
    set(outputs)
    foreach(entry IN LISTS entries)
        string(REPLACE "|" ";" fields "${entry}")
        list(LENGTH fields field_count)
        if(field_count LESS 2)
            message(FATAL_ERROR "[target_add_asset] invalid entry '${entry}'")
        endif()
        list(GET fields 0 type)
        list(GET fields 1 source)
        if(field_count GREATER 2)
            list(GET fields 2 relative_output)
        else()
            get_filename_component(base "${source}" NAME_WE)
            set(relative_output "${base}.luxasset")
        endif()
        if(NOT type)
            set(type "${default_type}")
        endif()
        if(NOT type)
            message(FATAL_ERROR "[target_add_asset] entry '${entry}' has no type")
        endif()

        set(output "${out_dir}/${relative_output}")
        get_filename_component(output_parent "${output}" DIRECTORY)
        file(MAKE_DIRECTORY "${output_parent}")
        set(extra_arguments)
        if(type STREQUAL "texture")
            if(texture_format)
                list(APPEND extra_arguments --texture_format "${texture_format}")
            endif()
            if(texture_color_space)
                list(APPEND extra_arguments --texture_color_space "${texture_color_space}")
            endif()
        endif()

        if(inspect)
            add_custom_command(
                OUTPUT "${output}"
                COMMAND ${packer}
                    --type "${type}" --source_path "${source}" --target_path "${output}" ${extra_arguments}
                COMMAND ${packer} --inspect --target_path "${output}"
                DEPENDS "${source}"
                VERBATIM
            )
        else()
            add_custom_command(
                OUTPUT "${output}"
                COMMAND ${packer}
                    --type "${type}" --source_path "${source}" --target_path "${output}" ${extra_arguments}
                DEPENDS "${source}"
                VERBATIM
            )
        endif()
        list(APPEND outputs "${output}")
    endforeach()

    set(generation_target "${ASSET_NAME}_generate")
    if(NOT TARGET ${generation_target})
        add_custom_target(${generation_target} DEPENDS ${outputs})
    endif()
    if(packer_target)
        add_dependencies(${generation_target} ${packer_target})
    endif()
    add_dependencies(${ASSET_TARGET} ${generation_target})
endfunction()
