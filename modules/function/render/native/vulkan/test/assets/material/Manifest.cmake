execute_process(COMMAND "${COMPILER}" --version OUTPUT_VARIABLE version RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Cannot identify material asset compiler")
endif()
file(WRITE "${OUTPUT}" "Unlit material fixture: existing MaterialGraph -> lowerMaterial -> ShaderEmitter -> glslang\n${version}\n")
foreach(path IN ITEMS
    "${INPUT}/EmitMaterial.cpp" "${INPUT}/material_types.glsl" "${INPUT}/texture_sampling.glsl"
    "${INPUT}/transition_dither.glsl" "${INPUT}/provenance.json"
    "${ROOT}/modules/function/material/src/MaterialLowering.cpp"
    "${ROOT}/engine/toolchain/material/src/ShaderEmitter.cpp"
    "${GENERATED}/Unlit.frag" "${GENERATED}/Unlit.frag.spv"
)
    file(SHA256 "${path}" hash)
    file(APPEND "${OUTPUT}" "${hash}  ${path}\n")
endforeach()
