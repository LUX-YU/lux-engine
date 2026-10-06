set(LUX_EDITOR_MIGRATION_STAGE P01 CACHE STRING "Current Editor migration gate (P00..P10, P10Q, P11..P13, EC1..EC4)")

# One explicit order shared with source and evidence validators. Unknown stages never fall back.
file(READ "${CMAKE_CURRENT_LIST_DIR}/tests/architecture/rules.json" _editor_rules)
string(JSON _stage_count LENGTH "${_editor_rules}" stages)
math(EXPR _stage_last "${_stage_count} - 1")
set(LUX_EDITOR_MIGRATION_STAGES "")
foreach(index RANGE ${_stage_last})
    string(JSON stage GET "${_editor_rules}" stages ${index})
    list(APPEND LUX_EDITOR_MIGRATION_STAGES "${stage}")
endforeach()
set_property(CACHE LUX_EDITOR_MIGRATION_STAGE PROPERTY STRINGS ${LUX_EDITOR_MIGRATION_STAGES})
if(NOT LUX_EDITOR_MIGRATION_STAGE IN_LIST LUX_EDITOR_MIGRATION_STAGES)
    message(FATAL_ERROR "Unknown Editor migration stage: ${LUX_EDITOR_MIGRATION_STAGE}")
endif()

