# Kept after Vulkan creation; the neutral render_data target never links Native.
set(f6_data "${CMAKE_CURRENT_LIST_DIR}/../../modules/function/render/data")
set(f6_native "${CMAKE_CURRENT_LIST_DIR}/../../modules/function/render/vulkan/test")
add_executable(render_data_gpu "${f6_data}/test/Gpu.cpp")

target_link_libraries(render_data_gpu PRIVATE render_data render_vulkan toolchain_shader_emitter toolchain_shader_reflection)

target_include_directories(render_data_gpu PRIVATE
    "${f6_native}" "${f6_data}/test" "${CMAKE_BINARY_DIR}/render_data/test/generated"
)

add_dependencies(render_data_gpu render_data_test_data_schema)

set(f6_shader "${CMAKE_CURRENT_BINARY_DIR}/data_shader")
file(MAKE_DIRECTORY "${f6_shader}")
add_custom_command(
    OUTPUT "${f6_shader}/Light.comp" "${f6_shader}/Sprite.comp" "${f6_shader}/Complex.comp" "${f6_shader}/Integer.comp"
    COMMAND render_data_gpu emit "${f6_shader}"
    DEPENDS render_data_gpu
    VERBATIM
)

foreach(name IN ITEMS Light Sprite Complex Integer)
    add_custom_command(
        OUTPUT "${f6_shader}/${name}.spv"
        COMMAND "${Vulkan_GLSLC_EXECUTABLE}" -fshader-stage=comp --target-env=vulkan1.3
            "${f6_shader}/${name}.comp" -o "${f6_shader}/${name}.spv"
        DEPENDS "${f6_shader}/${name}.comp"
        VERBATIM
    )
    list(APPEND f6_spirv "${f6_shader}/${name}.spv")
endforeach()

add_custom_target(render_data_spirv ALL DEPENDS ${f6_spirv})

add_test(NAME render.data.gpu_abi COMMAND render_data_gpu verify "${f6_shader}")
set_tests_properties(render.data.gpu_abi PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
# Original two-projection regression: exact existing test and templates, no aggregate Meta runtime.
find_package(lux-cxx CONFIG REQUIRED COMPONENTS serialization reflection_runtime)
include_component_cmake_scripts(lux::cxx::serialization)
set(f6_meta "${CMAKE_CURRENT_LIST_DIR}/../../modules/core/meta")
set(f6_type "${CMAKE_CURRENT_LIST_DIR}/../../modules/core/type_info/include")
include("${f6_meta}/cmake/engine_add_type_info.cmake")
add_executable(meta_projection_compatibility_test "${f6_meta}/test/ProjectionCompatibility.cpp")

target_include_directories(meta_projection_compatibility_test PRIVATE "${f6_type}" "${f6_meta}/test")

target_link_libraries(meta_projection_compatibility_test PRIVATE lux::cxx::serialization lux::cxx::reflection_runtime)

target_compile_options(meta_projection_compatibility_test PRIVATE /UNDEBUG /utf-8)

lux_add_codegen_job(
    NAME meta_projection_compatibility MARKER luxref
    SOURCE_FILE "${f6_meta}/test/ProjectionCompatibility.cpp"
    TARGET_FILES "${f6_meta}/test/ProjectionTypes.hpp" LOGICAL_PATHS ProjectionTypes.hpp
)

engine_type_static_info_template(f6_static_template)
lux_codegen_add_projection(
    JOB meta_projection_compatibility NAME engine_fields TEMPLATE "${f6_static_template}"
    OUTPUT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/projection_test" OUTPUT_SUFFIX .type_static_info.hpp
)

lux_codegen_add_projection(
    JOB meta_projection_compatibility NAME serialization_fields TEMPLATE "${LUX_SERIALIZATION_TEMPLATE}"
    OUTPUT_ROOT "${CMAKE_CURRENT_BINARY_DIR}/projection_test" OUTPUT_SUFFIX .serialize.hpp
)

lux_target_add_codegen(TARGET meta_projection_compatibility_test JOB meta_projection_compatibility DONT_ADD_TO_SOURCE)

add_test(NAME meta.projection_compatibility COMMAND meta_projection_compatibility_test)
include(CMakePackageConfigHelpers)
install(TARGETS render_data render_core shader_metadata
    EXPORT RenderDataTargets ARCHIVE DESTINATION lib COMPONENT RenderDataSDK
)

install(EXPORT RenderDataTargets NAMESPACE lux::engine::render::
    DESTINATION lib/cmake/RenderData COMPONENT RenderDataSDK
)

install(DIRECTORY "${f6_data}/include/" "${f6_data}/../core/include/"
    DESTINATION include COMPONENT RenderDataSDK
)

install(FILES "${CMAKE_CURRENT_LIST_DIR}/../../modules/resource/description/include/lux/engine/description/DataLayout.hpp"
    DESTINATION include/lux/engine/description COMPONENT RenderDataSDK
)

install(DIRECTORY "${f6_data}/cmake/" DESTINATION lib/cmake/RenderData/codegen COMPONENT RenderDataSDK)

install(FILES "${CMAKE_CURRENT_LIST_DIR}/../../engine/toolchain/shader/cmake/GpuValues.py"
    DESTINATION lib/cmake/RenderData/codegen COMPONENT RenderDataSDK
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/RenderDataConfig.cmake" [=[
include(CMakeFindDependencyMacro)
find_dependency(lux-cxx CONFIG COMPONENTS core container algorithm compile_time reflection_runtime)
include("${CMAKE_CURRENT_LIST_DIR}/RenderDataTargets.cmake")
set(LUX_DATA_GPU_TOOLS "${CMAKE_CURRENT_LIST_DIR}/codegen")
include("${CMAKE_CURRENT_LIST_DIR}/codegen/RenderDataCodegen.cmake")
]=])

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/RenderDataConfig.cmake"
    DESTINATION lib/cmake/RenderData COMPONENT RenderDataSDK
)

install(FILES
    "${CMAKE_CURRENT_LIST_DIR}/../../modules/core/error/include/lux/engine/error/Error.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../modules/core/error/include/lux/engine/error/ErrorDescriptor.hpp"
    DESTINATION include/lux/engine/error COMPONENT RenderDataSDK
)
