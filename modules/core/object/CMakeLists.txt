find_package(lux-cxx CONFIG REQUIRED COMPONENTS core compile_time memory)

add_component(
    COMPONENT_NAME object
    NAMESPACE lux::engine::core
    OUTPUT_NAME lux_engine_core_object
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Connection.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Object.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectDispatcher.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ObjectReflection.cpp
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
        meta
        lux::cxx::core
        lux::cxx::compile_time
        lux::cxx::memory
)

component_add_transitive_commands(
    object
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time memory)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
)

lux_classify_target(
    TARGET object
    LAYER CORE
    PRODUCT RUNTIME
    ROLE FOUNDATION
)
