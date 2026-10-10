execute_process(
    COMMAND "${EXECUTABLE}" lifetime borrow
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
)

if(NOT result STREQUAL "86")
    message(FATAL_ERROR "Live CPU borrow did not invoke the release-build fatal contract: ${result}\n${output}\n${errors}")
endif()

message(STATUS "Live CommandBatch borrower rejected by std::terminate handler, exit=86")
