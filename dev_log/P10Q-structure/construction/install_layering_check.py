from pathlib import Path
import json

repo=Path(r'E:/SyncForder/CodeRepos/lux-engine-p10q-structure')
work=Path(__file__).parent
(repo/'editor/tests/architecture/editor_layering.py').write_bytes((work/'editor_layering.py').read_bytes())
p=repo/'editor/tests/architecture/check_editor_boundaries.py';s=p.read_text()
s=s.replace('def inspect(repo, records, rules, stage, compile_db=None):','def inspect(repo, records, rules, stage, compile_db=None, layering_mode=None, compiler_dependencies=None):')
s=s.replace('    return findings\n','''    if layering_mode is not None:
        import editor_layering
        editor_layering.check(repo, records, sources, rules, layering_mode, report, compiler_dependencies)
    return findings
''')
s=s.replace('    args = parser.parse_args()','''    parser.add_argument("--layering-mode", choices=("CONSTRUCTION", "STRICT"))
    parser.add_argument("--compiler-dependencies", type=Path)
    args = parser.parse_args()''')
s=s.replace('args.stage, args.compile_db)','args.stage, args.compile_db, args.layering_mode, args.compiler_dependencies)')
p.write_text(s,newline='\n')
p=repo/'cmake/EditorArchitectureChecks.cmake';s=p.read_text()
needle='    lux_editor_collect_targets("${PROJECT_SOURCE_DIR}" targets)'
s=s.replace(needle,'''    if(DEFINED LUX_EDITOR_LAYERING_MODE)
        if(NOT LUX_EDITOR_LAYERING_MODE MATCHES "^(CONSTRUCTION|STRICT)$")
            message(FATAL_ERROR "Unknown LUX_EDITOR_LAYERING_MODE: ${LUX_EDITOR_LAYERING_MODE}")
        endif()
        # Inspect the already selected directory-local imported targets, never classify their
        # unresolved qualified names as native linker leaves.
        foreach(pair "filewatch;efsw" "physics2d_simulation;box2d" "navigation_detour3d;recastnavigation")
            list(GET pair 0 provider)
            list(GET pair 1 package)
            if(TARGET ${provider})
                find_package(${package} CONFIG REQUIRED PATHS "${${package}_DIR}" NO_DEFAULT_PATH)
            endif()
        endforeach()
    endif()
'''+needle)
s=s.replace('foreach(property SOURCE_DIR TYPE','foreach(property SOURCE_DIR BINARY_DIR TYPE')
needle='    execute_process(COMMAND "${Python3_EXECUTABLE}"'
s=s.replace(needle,'''    set(layering_arguments)
    if(DEFINED LUX_EDITOR_LAYERING_MODE)
        list(APPEND layering_arguments --layering-mode "${LUX_EDITOR_LAYERING_MODE}")
    endif()
'''+needle)
s=s.replace('--stage "${LUX_EDITOR_MIGRATION_STAGE}"\n        RESULT_VARIABLE','--stage "${LUX_EDITOR_MIGRATION_STAGE}" ${layering_arguments}\n        RESULT_VARIABLE')
p.write_text(s,newline='\n')
p=repo/'CMakeLists.txt';s=p.read_text()
s=s.replace('project(','''set(LUX_EDITOR_LAYERING_MODE "CONSTRUCTION" CACHE STRING "Editor source responsibility qualification")
set_property(CACHE LUX_EDITOR_LAYERING_MODE PROPERTY STRINGS CONSTRUCTION STRICT)
if(NOT LUX_EDITOR_LAYERING_MODE MATCHES "^(CONSTRUCTION|STRICT)$")
    message(FATAL_ERROR "Unknown LUX_EDITOR_LAYERING_MODE: ${LUX_EDITOR_LAYERING_MODE}")
endif()

project(''',1)
p.write_text(s,newline='\n')
p=repo/'editor/tests/architecture/CMakeLists.txt';s=p.read_text()
s=s.replace('--compile-db "${CMAKE_BINARY_DIR}/compile_commands.json" --stage "${LUX_EDITOR_MIGRATION_STAGE}")','--compile-db "${CMAKE_BINARY_DIR}/compile_commands.json" --stage "${LUX_EDITOR_MIGRATION_STAGE}"\n        --layering-mode "${LUX_EDITOR_LAYERING_MODE}")')
p.write_text(s,newline='\n')
