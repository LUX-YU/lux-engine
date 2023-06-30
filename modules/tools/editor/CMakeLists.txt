set(LUX_EDITOR_SRCS
    src/builtin-obj/BasicCamera.cpp
)

add_component(
    COMPONENT_NAME                  builtin_obj
    NAMESPACE                       lux::engine::tools
    SOURCE_FILES                    ${LUX_EDITOR_SRCS}
	BUILD_TIME_EXPORT_INCLUDE_DIRS  ${CMAKE_CURRENT_SOURCE_DIR}/include
                                    ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME_INCLUDE_PREFIX     include
    PUBLIC_LIBRARIES                lux::engine::platform::system
                                    lux::engine::core::meta
                                    lux::engine::core::math
    PRIVATE_DEFINITIONS             LUX_CORE_LIBRARY
    TRANSITIVE_PACKAGES_COMMANDS    "find_package (Eigen3 REQUIRED)"
)

add_executable(
    LuxEditor
    src/editor/LuxEditor.cpp
)

target_link_libraries(
    LuxEditor
    PRIVATE
    lux::engine::resource::asset
    lux::engine::function::render
    lux::engine::tools::builtin_obj
)