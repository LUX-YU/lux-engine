add_executable(render_data_test Schema.cpp)

target_link_libraries(render_data_test PRIVATE render_data)

target_include_directories(render_data_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

render_data_schema(render_data_test "${CMAKE_CURRENT_SOURCE_DIR}/Golden.hpp" SERIALIZATION)

add_test(NAME render.data.schema COMMAND render_data_test)

foreach(header IN ITEMS Schema Annotations)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header_${header}.cpp" "#include <lux/engine/render/data/${header}.hpp>\n")
    add_library(render_data_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header_${header}.cpp")
    target_link_libraries(render_data_header_${header} PRIVATE render_data)
endforeach()
find_package(lux-cxx CONFIG REQUIRED COMPONENTS serialization_json)

add_executable(render_data_wire Wire.cpp)

target_link_libraries(render_data_wire PRIVATE render_data lux::cxx::serialization_json)

target_include_directories(render_data_wire PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

add_dependencies(render_data_wire render_data_test_data_schema)

add_test(NAME render.data.wire COMMAND render_data_wire)

foreach(case IN ITEMS missing scope version borrow shape authority layout private wire nested)
    add_executable("render_data_reject_${case}" EXCLUDE_FROM_ALL negative/Main.cpp)
    target_link_libraries("render_data_reject_${case}" PRIVATE render_data)
    render_data_schema("render_data_reject_${case}" "${CMAKE_CURRENT_SOURCE_DIR}/negative/${case}.hpp")
    add_test(NAME "render.data.reject_${case}"
        COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DTARGET=render_data_reject_${case}"
            "-DEXPECTED=DataSchema rejected:" -P "${CMAKE_CURRENT_SOURCE_DIR}/Reject.cmake"
    )
    set_tests_properties("render.data.reject_${case}" PROPERTIES RUN_SERIAL TRUE)
endforeach()

foreach(case IN ITEMS GPU WIRE AUTHORITY METADATA VULKAN RUNTIME GRAPH TRANSPORT)
    add_executable("render_data_probe_${case}" EXCLUDE_FROM_ALL Probe.cpp)
    target_link_libraries("render_data_probe_${case}" PRIVATE render_data)
    target_include_directories("render_data_probe_${case}" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_definitions("render_data_probe_${case}" PRIVATE "REJECT_${case}")
    add_dependencies("render_data_probe_${case}" render_data_test_data_schema)
    add_test(NAME "render.data.reject_${case}"
        COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DTARGET=render_data_probe_${case}"
            "-DEXPECTED=error C(2338|2607|1083)" -P "${CMAKE_CURRENT_SOURCE_DIR}/Reject.cmake"
    )
    set_tests_properties("render.data.reject_${case}" PROPERTIES RUN_SERIAL TRUE)
endforeach()
add_executable(render_data_benchmark Benchmark.cpp)

target_link_libraries(render_data_benchmark PRIVATE render_data)

target_include_directories(render_data_benchmark PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${CMAKE_CURRENT_SOURCE_DIR}/../../vulkan/test"
)

add_dependencies(render_data_benchmark render_data_test_data_schema)

add_test(NAME render.data.pack_zero_allocation COMMAND render_data_benchmark)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header_DataLayout.cpp" "#include <lux/engine/description/DataLayout.hpp>\n")
add_library(render_data_header_DataLayout OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header_DataLayout.cpp")

target_link_libraries(render_data_header_DataLayout PRIVATE shader_metadata)

add_executable(render_data_codegen_identity CodegenIdentity.cpp)

target_link_libraries(render_data_codegen_identity PRIVATE render_data)

render_data_schema(render_data_codegen_identity "${CMAKE_CURRENT_SOURCE_DIR}/installed/a/Types.hpp" OUTPUT_NAME "a/Types")

render_data_schema(render_data_codegen_identity "${CMAKE_CURRENT_SOURCE_DIR}/installed/b/Types.hpp" OUTPUT_NAME "b/Types")

add_test(NAME render.data.codegen_identity COMMAND render_data_codegen_identity)
