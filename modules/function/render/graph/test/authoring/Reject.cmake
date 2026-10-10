execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${REJECT_TARGET}" -j 4 -- -k 0
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
)

file(WRITE "${BUILD_DIR}/${REJECT_TARGET}.log" "${output}\n${error}")

if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${EXPECTED}")
    message(FATAL_ERROR "Expected rejection was not observed: ${REJECT_TARGET}\n${output}\n${error}")
endif()
