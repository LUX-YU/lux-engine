execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "render_core_reject_${PROBE}" -j 4 -- -k 0
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
)

file(WRITE "${BUILD_DIR}/reject_${PROBE}.log" "${output}\n${errors}")

if(result EQUAL 0)
    message(FATAL_ERROR "Forbidden ${PROBE} consumer compiled")
endif()

if(PROBE STREQUAL "ID" OR PROBE STREQUAL "HANDLE")
    if(MSVC_PROBE)
        set(expected "C2440")
    else()
        set(expected "convert|conversion")
    endif()
else()
    if(MSVC_PROBE)
        set(expected "C1083")
    else()
        set(expected "No such file|file not found")
    endif()
    if(PROBE STREQUAL "VULKAN")
        set(missing_header "vulkan/vulkan.h")
    elseif(PROBE STREQUAL "SCENE")
        set(missing_header "lux/engine/scene/SceneSystem.hpp")
    elseif(PROBE STREQUAL "RUNTIME")
        set(missing_header "lux/engine/render/RenderRuntime.hpp")
    elseif(PROBE STREQUAL "LEGACY")
        set(missing_header "lux/engine/function/render/client/core/RenderResourceHandle.hpp")
    endif()
    string(FIND "${output}${errors}" "${missing_header}" header_position)
    if(header_position EQUAL -1)
        message(FATAL_ERROR "Probe did not diagnose the forbidden header: ${missing_header}")
    endif()
endif()

if(NOT "${output}${errors}" MATCHES "${expected}")
    message(FATAL_ERROR "Probe failed for an unexpected reason; see reject_${PROBE}.log")
endif()
