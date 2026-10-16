add_executable(render_resource_test "${CMAKE_CURRENT_SOURCE_DIR}/Storage.cpp")

target_link_libraries(render_resource_test PRIVATE render_resource)

add_test(NAME render.resource.contract COMMAND render_resource_test)

add_executable(render_resource_registry_test Registry.cpp)

target_link_libraries(render_resource_registry_test PRIVATE render_resource)

add_test(NAME render.resource.registry COMMAND render_resource_registry_test)

foreach(header Storage Factory Registration Texture TransferOptions ResourceRegistry CpuSchedule Upload)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cpp" CONTENT
        "#include <lux/engine/render/resource/${header}.hpp>\n#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native leak\n#endif\nstatic_assert(__cplusplus >= 202002L);\n"
    )

    add_library(render_resource_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cpp")

    target_link_libraries(render_resource_header_${header} PRIVATE render_resource)
endforeach()
