find_package(tomlplusplus CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RESOURCE_DEPLOYMENT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RESOURCE_DEPLOYMENT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RESOURCE_DEPLOYMENT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/resource/deployment/visibility.h
)

add_component(
    COMPONENT_NAME deployment
    NAMESPACE      lux::engine::resource
    OUTPUT_NAME    lux_engine_resource_deployment
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RuntimeCapacity.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RuntimeLaunchManifest.cpp
)

lux_classify_target(
    TARGET  deployment
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

component_include_directories(
    deployment
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    deployment
    PRIVATE LUX_ENGINE_RESOURCE_DEPLOYMENT_LIBRARY
)

target_link_libraries(
    deployment
    PUBLIC
        lux::cxx::compile_time
        lux::engine::core::extension_abi
    PRIVATE
        tomlplusplus::tomlplusplus
)

component_add_transitive_commands(
    deployment
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS extension_abi)"
)

add_executable(
    runtime_capacity_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/runtime_capacity_test.cpp
)
target_link_libraries(
    runtime_capacity_test
    PRIVATE lux::engine::resource::deployment
)
if(MSVC)
    target_compile_options(runtime_capacity_test PRIVATE /UNDEBUG)
else()
    target_compile_options(runtime_capacity_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  runtime_capacity_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME runtime_capacity_test COMMAND runtime_capacity_test)
set_tests_properties(
    runtime_capacity_test
    PROPERTIES LABELS "cpu;resource;capacity" TIMEOUT 30
)
