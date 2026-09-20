set(_feature_root "${CMAKE_CURRENT_LIST_DIR}/..")
foreach(_part IN ITEMS client meta)
    string(TOUPPER "${_part}" _upper)
    generate_visibility_header(
        ENABLE_MACRO_NAME LUX_RENDER_FEATURE_${_upper}_LIBRARY
        PUBLIC_MACRO_NAME LUX_RENDER_FEATURE_${_upper}_PUBLIC
        DISABLE_DLL_MACRO_NAME LUX_RENDER_FEATURE_${_upper}_DLL_DISABLE
        GENERATE_FILE_PATH lux/engine/function/render/features/${_part}_visibility.h)
endforeach()
generate_visibility_header(
    ENABLE_MACRO_NAME LUX_ENGINE_FUNCTION_RENDER_FEATURES_LIBRARY
    PUBLIC_MACRO_NAME LUX_ENGINE_FUNCTION_RENDER_FEATURES_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_ENGINE_FUNCTION_RENDER_FEATURES_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/function/render/features/visibility.h)

add_component(COMPONENT_NAME render_feature_client NAMESPACE lux::engine::function
    SOURCE_FILES
        ${_feature_root}/src/client/Canvas2DOperationClient.cpp
        ${_feature_root}/src/client/LightOperationClient.cpp
        ${_feature_root}/src/client/MaterialOperationClient.cpp
        ${_feature_root}/src/client/MeshStackOperationClient.cpp
        ${_feature_root}/src/client/ViewCameraOperation.cpp
        ${_feature_root}/src/client/RenderFeatureRegistrations.cpp
        ${_feature_root}/src/client/PointCloudRenderFeatureRegistrations.cpp)
component_include_directories(render_feature_client
    BUILD_TIME_EXPORT ${_feature_root}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include)
target_compile_definitions(render_feature_client PRIVATE LUX_RENDER_FEATURE_CLIENT_LIBRARY)
target_link_libraries(render_feature_client PUBLIC lux::engine::function::render_client)
component_add_internal_dependencies(render_feature_client render_client)

include_component_cmake_scripts(meta)
include(${_feature_root}/../cmake/engine_render_codegen.cmake)
include(${_feature_root}/cmake/render_comm_operations.cmake)
engine_add_comm_ops(NAME render_comm_ops CLIENT_TARGET render_feature_client
    INCLUDE_PREFIX lux/engine/function/render/features/genops
    CLIENT_EXPORT_MACRO LUX_RENDER_FEATURE_CLIENT_PUBLIC
    CLIENT_VISIBILITY_HEADER lux/engine/function/render/features/client_visibility.h
    BACKEND_EXPORT_MACRO LUX_ENGINE_FUNCTION_RENDER_FEATURES_PUBLIC
    BACKEND_VISIBILITY_HEADER lux/engine/function/render/features/visibility.h
    TARGET_FILES ${LUX_RENDER_COMM_OPERATION_HEADERS})

add_component(COMPONENT_NAME render_feature_meta NAMESPACE lux::engine::function
    OUTPUT_NAME lux_engine_function_render_feature_meta
    SOURCE_FILES ${_feature_root}/src/client/RenderFeatureMetaModule.cpp)
component_include_directories(render_feature_meta
    BUILD_TIME_EXPORT ${_feature_root}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include)
target_compile_definitions(render_feature_meta PRIVATE LUX_RENDER_FEATURE_META_LIBRARY)
target_link_libraries(render_feature_meta PRIVATE render_feature_client lux::engine::core::meta)
component_add_internal_dependencies(render_feature_meta render_feature_client)
engine_enable_module_meta(TARGET render_feature_client SIDECAR_TARGET render_feature_meta
    REGISTER_FUNC_MACRO LUX_RENDER_FEATURE_META_PUBLIC
    VISIBILITY_HEADER lux/engine/function/render/features/meta_visibility.h
    TARGET_FILES ${LUX_RENDER_COMM_OPERATION_HEADERS}
        ${_feature_root}/include/lux/engine/function/render/features/postprocess/TonemapParams.hpp
        ${_feature_root}/include/lux/engine/function/render/features/spatialcull/SpatialCullParams.hpp
        ${_feature_root}/include/lux/engine/function/render/features/shadow/ShadowQualityParams.hpp)
