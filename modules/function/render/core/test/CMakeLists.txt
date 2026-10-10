add_executable(render_core_test Core.cpp)

target_link_libraries(render_core_test PRIVATE lux::engine::render::render_core)

add_test(NAME render.core.values COMMAND render_core_test)

# Each public header must compile as the first and only include in its own TU.
foreach(header IN ITEMS Identity Handles Error Descriptors Target)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_${header}.cpp")
    file(WRITE "${source}" "#include <lux/engine/render/core/${header}.hpp>\n")
    add_library(render_core_header_${header} OBJECT "${source}")
    target_link_libraries(render_core_header_${header} PRIVATE lux::engine::render::render_core)
endforeach()

add_executable(render_core_probe_control Probe.cpp)

target_link_libraries(render_core_probe_control PRIVATE lux::engine::render::render_core)

add_test(NAME render.core.probe_control COMMAND render_core_probe_control)

foreach(probe IN ITEMS ID HANDLE VULKAN SCENE RUNTIME LEGACY)
    add_executable(render_core_reject_${probe} EXCLUDE_FROM_ALL Probe.cpp)
    target_compile_definitions(render_core_reject_${probe} PRIVATE LUX_PROBE_${probe})
    target_link_libraries(render_core_reject_${probe} PRIVATE lux::engine::render::render_core)
    add_test(
        NAME render.core.reject_${probe}
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DPROBE=${probe}"
            "-DMSVC_PROBE=${MSVC}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/RejectCompile.cmake"
    )
    set_tests_properties(render.core.reject_${probe} PROPERTIES RUN_SERIAL TRUE)
endforeach()
