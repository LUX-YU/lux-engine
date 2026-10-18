add_executable(platform_ipc_test Ipc.cpp Allocation.cpp)

target_link_libraries(platform_ipc_test PRIVATE platform_ipc render_commands)
target_include_directories(platform_ipc_test PRIVATE ../pinclude)

add_executable(platform_ipc_attachments_test Attachments.cpp)
find_package(lux-cxx CONFIG REQUIRED COMPONENTS memory)
target_link_libraries(platform_ipc_attachments_test PRIVATE platform_ipc lux::cxx::memory)
target_include_directories(platform_ipc_attachments_test PRIVATE ../pinclude)
set_target_properties(platform_ipc_attachments_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
add_test(NAME platform.ipc.attachments COMMAND platform_ipc_attachments_test)
set_tests_properties(platform.ipc.attachments PROPERTIES TIMEOUT 30)

# Cross-DLL owned values use the same allocation instrumentation as their EXE producer.
add_library(ipc_typed_plugin SHARED TypedPlugin.cpp Allocation.cpp AllocationOrigin.cpp)

target_link_libraries(ipc_typed_plugin PRIVATE platform_ipc)
set_target_properties(ipc_typed_plugin PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin"
)

add_executable(platform_ipc_typed_test TypedMessage.cpp Allocation.cpp AllocationOrigin.cpp)

target_link_libraries(platform_ipc_typed_test PRIVATE platform_ipc)
target_include_directories(platform_ipc_typed_test PRIVATE ../pinclude)
add_dependencies(platform_ipc_typed_test ipc_typed_plugin)
set_target_properties(platform_ipc_typed_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
add_test(NAME platform.ipc.typed COMMAND platform_ipc_typed_test)
set_tests_properties(platform.ipc.typed PROPERTIES TIMEOUT 30)

foreach(plugin A B)
    add_library(ipc_plugin_${plugin} SHARED Plugin${plugin}.cpp)
    target_link_libraries(ipc_plugin_${plugin} PRIVATE platform_ipc)
    set_target_properties(ipc_plugin_${plugin} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_link_libraries(platform_ipc_test PRIVATE ipc_plugin_${plugin})
endforeach()

foreach(target platform_ipc platform_ipc_test ipc_plugin_A ipc_plugin_B)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
endforeach()

foreach(test lifecycle messages directory grace closeout fifo progress protocol cost)
    add_test(NAME platform.ipc.${test} COMMAND platform_ipc_test ${test})
    set_tests_properties(platform.ipc.${test} PROPERTIES TIMEOUT 90)
endforeach()

foreach(header IpcCore IpcMessage)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp" CONTENT
        "#include <lux/engine/platform/ipc/${header}.hpp>\n#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native leak\n#endif\nstatic_assert(__cplusplus >= 202002L);\n")
    add_library(ipc_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")
    target_link_libraries(ipc_header_${header} PRIVATE platform_ipc)
endforeach()

add_test(
    NAME platform.ipc.installed
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
