add_library(editor_session_types INTERFACE)
add_library(lux::engine::editor::editor_session_types ALIAS editor_session_types)
target_include_directories(editor_session_types INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)

install(DIRECTORY include/ DESTINATION include)
install(TARGETS editor_session_types EXPORT editor_session_types_export)
install(EXPORT editor_session_types_export FILE lux-engine-editor-session-types-config.cmake
    NAMESPACE lux::engine::editor:: DESTINATION lib/cmake/lux-engine-editor-session-types)
