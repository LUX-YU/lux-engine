if(NOT EXISTS "${PACKER}" OR NOT EXISTS "${TEXTURE_SOURCE}" OR NOT EXISTS "${SHADER_SOURCE}")
    message(FATAL_ERROR "packer smoke inputs are missing")
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
file(MAKE_DIRECTORY "${OUTPUT_ROOT}/content")

set(texture "${OUTPUT_ROOT}/content/checker.luxasset")
set(shader "${OUTPUT_ROOT}/content/minimal.luxasset")
set(pak "${OUTPUT_ROOT}/content.luxpak")

execute_process(
    COMMAND "${PACKER}"
        --type texture
        --source_path "${TEXTURE_SOURCE}"
        --target_path "${texture}"
        --texture_format bc3_srgb
        --texture_color_space srgb
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "texture pack failed: ${result}")
endif()

execute_process(
    COMMAND "${PACKER}"
        --type shader
        --source_path "${SHADER_SOURCE}"
        --target_path "${shader}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "shader pack failed: ${result}")
endif()

foreach(asset IN ITEMS "${texture}" "${shader}")
    execute_process(COMMAND "${PACKER}" --inspect --target_path "${asset}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "asset inspect failed: ${asset}: ${result}")
    endif()
endforeach()

execute_process(
    COMMAND "${PACKER}" --pack --source_path "${OUTPUT_ROOT}/content" --target_path "${pak}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Pak pack failed: ${result}")
endif()
execute_process(COMMAND "${PACKER}" --pak_inspect --target_path "${pak}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Pak inspect failed: ${result}")
endif()

file(SHA256 "${texture}" first_hash)
execute_process(
    COMMAND "${PACKER}"
        --type texture
        --source_path "${TEXTURE_SOURCE}"
        --target_path "${texture}"
        --texture_format bc3_srgb
        --texture_color_space srgb
    RESULT_VARIABLE result
)
file(SHA256 "${texture}" second_hash)
if(NOT result EQUAL 0 OR NOT first_hash STREQUAL second_hash)
    message(FATAL_ERROR "texture pack is not deterministic")
endif()
