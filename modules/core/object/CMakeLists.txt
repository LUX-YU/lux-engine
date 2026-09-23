find_package(lux-cxx CONFIG REQUIRED COMPONENTS core compile_time memory)

add_component(
    COMPONENT_NAME object
    NAMESPACE lux::engine::core
    OUTPUT_NAME lux_engine_core_object
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Object.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectDispatcher.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectWeakRef.cpp
)

component_include_directories(
    object
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE pinclude
)

target_compile_definitions(object PRIVATE LUX_CORE_LIBRARY)

option(
    LUX_OBJECT_CONTRACT_CHECKS
    "Enable Object affinity/lifetime contract checks outside Debug builds"
    OFF
)
if(LUX_OBJECT_CONTRACT_CHECKS)
    target_compile_definitions(object PUBLIC LUX_OBJECT_CONTRACT_CHECKS=1)
endif()

target_link_libraries(
    object
    PUBLIC
        lux::cxx::core
        lux::cxx::compile_time
        lux::cxx::memory
)

component_add_transitive_commands(
    object
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time memory)"
)

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_OBJECT_REFLECTION_LIBRARY
    PUBLIC_MACRO_NAME LUX_OBJECT_REFLECTION_PUBLIC
    GENERATE_FILE_PATH lux/engine/object/reflection_visibility.h
)
add_component(
    COMPONENT_NAME object_reflection
    NAMESPACE lux::engine::core
    SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectReflection.cpp
)
component_include_directories(object_reflection
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE pinclude
)
lux_classify_target(TARGET object_reflection LAYER MODULES PRODUCT SDK ROLE LIBRARY)
target_compile_definitions(object_reflection PRIVATE LUX_OBJECT_REFLECTION_LIBRARY)
target_link_libraries(object_reflection PUBLIC object meta)
component_add_transitive_commands(object_reflection
    "find_package(lux-engine-core REQUIRED COMPONENTS object meta)"
)
