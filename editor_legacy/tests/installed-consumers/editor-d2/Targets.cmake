add_library(consumer_domain SHARED ${D2_CONSUMER_SOURCE_DIR}/Domain.cpp)
target_compile_definitions(consumer_domain PRIVATE CONSUMER_DOMAIN_LIBRARY)
target_include_directories(consumer_domain PUBLIC "${D2_CONSUMER_SOURCE_DIR}/include" "${LUX_GENERATE_HEADER_DIR}")
target_link_libraries(consumer_domain PUBLIC lux::engine::simulation::ecs::schema)
engine_target_add_ecs_component_codegen(NAME consumer_schema TARGET consumer_domain
    SOURCE_FILE "${D2_CONSUMER_SOURCE_DIR}/Domain.cpp"
    HEADER "${D2_CONSUMER_SOURCE_DIR}/include/consumer/Component.hpp"
    LOGICAL_PATH consumer/Component.hpp SYMBOL Consumer)

add_library(consumer_gui SHARED ${D2_CONSUMER_SOURCE_DIR}/Gui.cpp ${D2_CONSUMER_SOURCE_DIR}/GuiGesture.cpp)
if(NOT LUX_TEST_SUPPORT_DIR)
    get_filename_component(LUX_TEST_SUPPORT_DIR "${D2_CONSUMER_SOURCE_DIR}/../../test-support" ABSOLUTE)
endif()
target_include_directories(consumer_gui PRIVATE "${LUX_TEST_SUPPORT_DIR}")
set_property(TARGET consumer_gui PROPERTY LUX_ARCH_LAYER EDITOR)
target_compile_definitions(consumer_gui PRIVATE CONSUMER_GUI_LIBRARY)
target_link_libraries(consumer_gui PUBLIC consumer_domain lux::engine::editor::scene_ui)
engine_target_add_imgui_inspector_codegen(NAME consumer TARGET consumer_gui
    SOURCE_FILE "${D2_CONSUMER_SOURCE_DIR}/Gui.cpp"
    HEADER "${D2_CONSUMER_SOURCE_DIR}/include/consumer/Component.hpp"
    LOGICAL_PATH consumer/Component.hpp COMPONENTS consumer::Component)
add_dependencies(consumer_gui consumer_domain)

add_executable(consumer_protocol ${D2_CONSUMER_SOURCE_DIR}/main.cpp)
target_link_libraries(consumer_protocol PRIVATE consumer_domain consumer_gui)
foreach(target consumer_domain consumer_gui consumer_protocol)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /UNDEBUG /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr /bigobj)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
endforeach()
