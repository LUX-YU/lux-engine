find_package(lux-cxx REQUIRED COMPONENTS arguments)
add_executable(lux_editor main.cpp)
target_link_libraries(lux_editor PRIVATE lux::engine::editor::editor_core
    lux::engine::editor::editor_gui lux::cxx::arguments)
if(WIN32)
    target_link_libraries(lux_editor PRIVATE comdlg32)
endif()
install(TARGETS lux_editor RUNTIME DESTINATION bin)
