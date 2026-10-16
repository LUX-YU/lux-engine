file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Protocol.cpp" CONTENT
    "#include <lux/engine/render/protocol/Protocol.hpp>\n#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native header leaked\n#endif\nstatic_assert(__cplusplus >= 202002L);\n")

add_library(render_protocol_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/Protocol.cpp")

target_link_libraries(render_protocol_header PRIVATE render_protocol)

add_executable(render_protocol_command_test Command.cpp)

target_link_libraries(render_protocol_command_test PRIVATE render_protocol)

add_test(NAME render.protocol.command COMMAND render_protocol_command_test)

get_target_property(protocol_links render_protocol INTERFACE_LINK_LIBRARIES)
set(expected_links lux::engine::render::render_core lux::cxx::core STDEXEC::stdexec)

if(NOT protocol_links STREQUAL expected_links)
    message(FATAL_ERROR "Unexpected Protocol dependency: ${protocol_links}")
endif()

add_test(
    NAME render.protocol.installed
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

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/AsyncCommand.cpp" CONTENT
    "#include <lux/engine/render/protocol/AsyncCommand.hpp>\n#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native leak\n#endif\n")

add_library(render_protocol_async_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/AsyncCommand.cpp")
target_link_libraries(render_protocol_async_header PRIVATE render_protocol)
