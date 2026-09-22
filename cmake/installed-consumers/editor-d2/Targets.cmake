add_library(consumer_domain SHARED ${D2_CONSUMER_SOURCE_DIR}/Domain.cpp)
target_compile_definitions(consumer_domain PRIVATE CONSUMER_DOMAIN_LIBRARY)
target_include_directories(consumer_domain PUBLIC "${D2_CONSUMER_SOURCE_DIR}/include" "${LUX_GENERATE_HEADER_DIR}")
target_link_libraries(consumer_domain PUBLIC lux::engine::simulation::ecs::schema)
engine_target_add_ecs_component_codegen(NAME consumer_schema TARGET consumer_domain
    SOURCE_FILE "${D2_CONSUMER_SOURCE_DIR}/Domain.cpp"
    HEADER "${D2_CONSUMER_SOURCE_DIR}/include/consumer/Component.hpp"
    LOGICAL_PATH consumer/Component.hpp SYMBOL Consumer)

add_library(consumer_gui SHARED ${D2_CONSUMER_SOURCE_DIR}/Gui.cpp ${D2_CONSUMER_SOURCE_DIR}/GuiGesture.cpp)
set_property(TARGET consumer_gui PROPERTY LUX_ARCH_LAYER EDITOR)
target_compile_definitions(consumer_gui PRIVATE CONSUMER_GUI_LIBRARY)
target_link_libraries(consumer_gui PUBLIC consumer_domain lux::engine::editor::editor_scene_ui)
engine_target_add_imgui_inspector_codegen(NAME consumer TARGET consumer_gui
    SOURCE_FILE "${D2_CONSUMER_SOURCE_DIR}/Gui.cpp"
    HEADER "${D2_CONSUMER_SOURCE_DIR}/include/consumer/Component.hpp"
    LOGICAL_PATH consumer/Component.hpp COMPONENTS consumer::Component)
add_dependencies(consumer_gui consumer_domain)

add_executable(consumer_protocol ${D2_CONSUMER_SOURCE_DIR}/main.cpp ${D2_CONSUMER_SOURCE_DIR}/SceneWorkflow.cpp)
target_link_libraries(consumer_protocol PRIVATE consumer_domain consumer_gui lux::engine::editor::editor_app)
option(D2_MEASURE_COMPONENT_COPIES "Instrument only the test component, never normal timing artifacts" OFF)
set(D2_EXPECT_ADOPT_COPIES 1 CACHE STRING "One after-value capture at field completion")
foreach(target consumer_domain consumer_gui consumer_protocol)
    if(D2_MEASURE_COMPONENT_COPIES)
        target_compile_definitions(${target} PRIVATE CONSUMER_MEASURE_COPIES
            CONSUMER_EXPECT_ADOPT_COPIES=${D2_EXPECT_ADOPT_COPIES})
        if(D2_EXPECT_ADOPT_COPIES STREQUAL "1")
            target_compile_definitions(${target} PRIVATE CONSUMER_WIDGET_DIAGNOSTICS)
        endif()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /UNDEBUG /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr)
    else()
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
endforeach()

