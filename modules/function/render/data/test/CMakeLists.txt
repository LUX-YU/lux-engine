add_executable(render_data_test Data.cpp)

target_link_libraries(render_data_test PRIVATE render_data)

foreach(test schema roundtrip malformed references ownership contracts)
    add_test(NAME render.data.${test} COMMAND render_data_test ${test})
endforeach()

foreach(header Schema Encoding Payload Shader ShaderInterface)
    string(CONCAT probe
        "#include <lux/engine/render/data/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
        "#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native header leaked into Data\n#endif\n"
    )

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cpp" CONTENT "${probe}")

    add_library(render_data_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cpp")

    target_link_libraries(render_data_header_${header} PRIVATE render_data)
endforeach()

get_target_property(data_links render_data INTERFACE_LINK_LIBRARIES)
set(expected_links lux::engine::render::render_core lux::cxx::core lux::cxx::binary lux::cxx::memory)
if(NOT data_links STREQUAL expected_links)
    message(FATAL_ERROR "Unexpected Data public dependency: ${data_links}")
endif()

file(
    GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/data-dependencies.txt"
    CONTENT "public_links=$<TARGET_PROPERTY:render_data,INTERFACE_LINK_LIBRARIES>\n"
)

add_test(
    NAME render.data.installed
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DCONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/installed"
        "-DDEPENDENCY_DIR=${lux-cxx_DIR}"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCONFIG=$<CONFIG>"
        "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../../core/test/Installed.cmake"
)

add_executable(render_data_borrowed_test Borrowed.cpp)

target_link_libraries(render_data_borrowed_test PRIVATE render_data)
add_test(NAME render.data.borrowed COMMAND render_data_borrowed_test)

add_executable(render_data_payload_test "${CMAKE_CURRENT_SOURCE_DIR}/Payload.cpp")

target_link_libraries(render_data_payload_test PRIVATE render_data)

add_library(render_data_payload_plugin SHARED PayloadPlugin.cpp)

target_compile_features(render_data_payload_plugin PRIVATE cxx_std_20)

set_target_properties(render_data_payload_plugin PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/data/bin"
)

set_target_properties(render_data_payload_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/data/bin")

add_dependencies(render_data_payload_test render_data_payload_plugin)

add_test(NAME render.data.payload COMMAND render_data_payload_test)
