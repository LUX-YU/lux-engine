include(CMakeFindDependencyMacro)
find_dependency(lux-engine-render CONFIG COMPONENTS render_extensions)
find_dependency(imgui CONFIG COMPONENTS core)
include("${CMAKE_CURRENT_LIST_DIR}/LuxEditorUiRender.cmake")
