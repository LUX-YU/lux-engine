add_executable(render_data_test Data.cpp)

target_link_libraries(render_data_test PRIVATE render_data)

foreach(test schema roundtrip malformed references ownership contracts)
    add_test(NAME render.data.${test} COMMAND render_data_test ${test})
endforeach()

foreach(header Schema Wire)
    string(CONCAT probe
        "#include <lux/engine/render/data/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
        "#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native header leaked into Data\n#endif\n"
    )

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp" CONTENT "${probe}")

    add_library(render_data_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")

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
