add_executable(render_core_test Core.cpp)

target_link_libraries(render_core_test PRIVATE render_core)

foreach(test ids scopes exhaustion target epochs errors)
    add_test(NAME render.core.${test} COMMAND render_core_test ${test})
endforeach()

set(core_header_sources)
foreach(header Ids Error Target Camera)
    set(probe "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cpp")
    string(CONCAT header_probe
        "#include <lux/engine/render/core/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
        "#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native header leaked into Core\n#endif\n"
    )

    file(GENERATE OUTPUT "${probe}" CONTENT "${header_probe}")
    list(APPEND core_header_sources "${probe}")
endforeach()

add_library(render_core_headers OBJECT ${core_header_sources})

target_link_libraries(render_core_headers PRIVATE render_core)

set_target_properties(render_core_headers PROPERTIES UNITY_BUILD OFF)

get_target_property(core_links render_core INTERFACE_LINK_LIBRARIES)
set(expected_links lux::cxx::container lux::cxx::compile_time lux::cxx::algorithm)
if(NOT core_links STREQUAL expected_links)
    message(FATAL_ERROR "Unexpected Core public dependency: ${core_links}")
endif()

string(CONCAT core_dependencies
    "public_links=$<TARGET_PROPERTY:render_core,INTERFACE_LINK_LIBRARIES>\n"
    "public_includes=$<TARGET_PROPERTY:render_core,INTERFACE_INCLUDE_DIRECTORIES>\n"
)

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/core-dependencies.txt" CONTENT "${core_dependencies}")

add_test(
    NAME render.core.installed
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DCONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/installed"
        "-DDEPENDENCY_DIR=${lux-cxx_DIR}"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCONFIG=$<CONFIG>"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/Installed.cmake"
)
