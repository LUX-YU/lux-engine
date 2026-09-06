find_package(assimp CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_path(LUX_MODEL_STB_INCLUDE_DIR NAMES stb_image.h REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS algorithm)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_MODEL_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_MODEL_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_TOOLCHAIN_MODEL_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/toolchain/asset/model/visibility.h
)

add_component(
    STATIC
    COMPONENT_NAME toolchain_model_cooker
    NAMESPACE      lux::engine::toolchain
    OUTPUT_NAME    lux_engine_toolchain_model_cooker
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ModelCooker.cpp
)
component_include_directories(
    toolchain_model_cooker
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${LUX_MODEL_STB_INCLUDE_DIR}
)
target_compile_definitions(
    toolchain_model_cooker
    PUBLIC LUX_ENGINE_TOOLCHAIN_MODEL_DLL_DISABLE
    PRIVATE LUX_ENGINE_TOOLCHAIN_MODEL_LIBRARY
)
target_link_libraries(
    toolchain_model_cooker
    PUBLIC
        lux::engine::resource::asset
        lux::engine::toolchain::toolchain_material_cooker
        lux::engine::toolchain::toolchain_texture_cooker
    PRIVATE
        assimp::assimp
        meshoptimizer::meshoptimizer
        lux::cxx::algorithm
)
component_add_transitive_commands(
    toolchain_model_cooker
    "find_package(assimp CONFIG REQUIRED)"
    "find_package(meshoptimizer CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-toolchain-material REQUIRED COMPONENTS toolchain_material_cooker)"
    "find_package(lux-engine-toolchain-asset REQUIRED COMPONENTS toolchain_texture_cooker)"
)
lux_classify_target(
    TARGET  toolchain_model_cooker
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)

if(WIN32 AND LUX_PROFILE_HAS_TOOLCHAIN)
    get_target_property(lux_assimp_runtime assimp::assimp IMPORTED_LOCATION_RELEASE)
    if(NOT lux_assimp_runtime)
        get_target_property(lux_assimp_runtime assimp::assimp IMPORTED_LOCATION)
    endif()
    get_filename_component(lux_assimp_runtime_dir "${lux_assimp_runtime}" DIRECTORY)
    set(lux_assimp_runtime_dependencies)
    foreach(runtime_name IN ITEMS poly2tri.dll minizip.dll zlib1.dll kubazip.dll pugixml.dll)
        unset(runtime_dependency)
        unset(runtime_dependency CACHE)
        find_file(
            runtime_dependency
            NAMES ${runtime_name}
            PATHS "${lux_assimp_runtime_dir}"
            NO_DEFAULT_PATH
            REQUIRED
        )
        list(APPEND lux_assimp_runtime_dependencies "${runtime_dependency}")
    endforeach()
    install(
        FILES ${lux_assimp_runtime_dependencies}
        DESTINATION bin
        COMPONENT lux_toolchain
    )
endif()
