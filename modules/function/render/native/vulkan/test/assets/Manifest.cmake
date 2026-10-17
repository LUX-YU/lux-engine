execute_process(COMMAND "${COMPILER}" --version OUTPUT_VARIABLE version RESULT_VARIABLE status)

if(NOT status EQUAL 0)
    message(FATAL_ERROR "Cannot identify the actual Shader compiler")
endif()

file(WRITE "${OUTPUT}" "compiler-version:\n${version}\nsha256  path\n")

foreach(path IN ITEMS
    "${COMPILER}" "${EMITTER}"
    "${ROOT}/engine/toolchain/shader/src/lglsl/LglslEmitter.cpp"
    "${INPUT}/EmitPass.cpp" "${INPUT}/Parameters.vert.lglsl"
    "${INPUT}/Parameters.frag.lglsl" "${INPUT}/Parameters.shared.lglslh"
    "${GENERATED}/Parameters.vert" "${GENERATED}/Parameters.frag"
    "${GENERATED}/Parameters.vert.spv" "${GENERATED}/Parameters.frag.spv"
)
    file(SHA256 "${path}" digest)
    file(APPEND "${OUTPUT}" "${digest}  ${path}\n")
endforeach()
