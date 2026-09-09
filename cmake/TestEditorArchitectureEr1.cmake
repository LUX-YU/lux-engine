file(MAKE_DIRECTORY "${LUX_ER1_TEST_DIR}")
set(probe "${LUX_ER1_TEST_DIR}/boundary-probe.cpp")
file(WRITE "${probe}" "#include <lux/engine/editor/rendering/EditorRenderer.hpp>\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -DLUX_SOURCE_DIR=${LUX_SOURCE_DIR}
    -DLUX_ER1_PROBE_FILE=${probe} -DLUX_ER1_PROBE_ROLE=rendering
    -P "${LUX_SOURCE_DIR}/cmake/ValidateEditorArchitectureEr1.cmake"
    RESULT_VARIABLE good OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT good EQUAL 0)
    message(FATAL_ERROR "Valid boundary fixture failed: ${out}${err}")
endif()
file(WRITE "${probe}" "#include <lux/engine/editor/scene/SceneWorkbench.hpp>\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -DLUX_SOURCE_DIR=${LUX_SOURCE_DIR}
    -DLUX_ER1_PROBE_FILE=${probe} -DLUX_ER1_PROBE_ROLE=rendering
    -P "${LUX_SOURCE_DIR}/cmake/ValidateEditorArchitectureEr1.cmake"
    RESULT_VARIABLE bad OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(bad EQUAL 0 OR NOT "${err}" MATCHES "ER1_BOUNDARY: rendering includes retired Editor dependency")
    message(FATAL_ERROR "Boundary fixture was not rejected for the required reason: ${bad} ${out}${err}")
endif()
message(STATUS "ER-1 positive and negative boundary fixtures passed")
