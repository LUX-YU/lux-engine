if(NOT DEFINED PACKER OR NOT DEFINED MODEL_SOURCE OR NOT DEFINED OUTPUT_ROOT OR NOT DEFINED PAK)
    message(FATAL_ERROR "Model packer smoke requires PACKER, MODEL_SOURCE, OUTPUT_ROOT, and PAK")
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}" "${OUTPUT_ROOT}.staging" "${OUTPUT_ROOT}.backup")
file(REMOVE "${PAK}")

execute_process(
    COMMAND "${PACKER}"
        --type model
        --source_path "${MODEL_SOURCE}"
        --target_path "${OUTPUT_ROOT}"
    RESULT_VARIABLE cook_result
)
if(NOT cook_result EQUAL 0)
    message(FATAL_ERROR "model cook failed: ${cook_result}")
endif()

file(GLOB_RECURSE cooked_assets "${OUTPUT_ROOT}/*.luxasset")
list(LENGTH cooked_assets cooked_count)
if(cooked_count LESS 9)
    message(FATAL_ERROR "model cook emitted only ${cooked_count} Assets")
endif()

file(GLOB model_assets "${OUTPUT_ROOT}/model/*.luxasset")
list(LENGTH model_assets model_count)
if(NOT model_count EQUAL 1)
    message(FATAL_ERROR "model cook must emit exactly one ModelAsset")
endif()
list(GET model_assets 0 model_asset)
execute_process(
    COMMAND "${PACKER}" --inspect --target_path "${model_asset}"
    RESULT_VARIABLE inspect_result
)
if(NOT inspect_result EQUAL 0)
    message(FATAL_ERROR "typed ModelAsset inspect failed: ${inspect_result}")
endif()

execute_process(
    COMMAND "${PACKER}"
        --pack
        --source_path "${OUTPUT_ROOT}"
        --target_path "${PAK}"
        --mount_hint /Game
    RESULT_VARIABLE pack_result
)
if(NOT pack_result EQUAL 0)
    message(FATAL_ERROR "model Pak build failed: ${pack_result}")
endif()
execute_process(
    COMMAND "${PACKER}" --pak_inspect --target_path "${PAK}"
    RESULT_VARIABLE pak_inspect_result
)
if(NOT pak_inspect_result EQUAL 0)
    message(FATAL_ERROR "model Pak inspect failed: ${pak_inspect_result}")
endif()
