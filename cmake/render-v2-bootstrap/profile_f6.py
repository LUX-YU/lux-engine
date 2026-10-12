"""Bounded VTune-only harness derived from frozen test sources, never production edits."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def digest(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def prepare(source, output, vtune):
    output.mkdir(parents=True, exist_ok=False)
    native = source / 'modules/function/render/vulkan/test/NativeGraphBenchmark.cpp'
    text = native.read_text()
    anchor = '    for (unsigned serial = 0; serial < 110; ++serial)'
    assert text.count(anchor) == 1
    text = '#include <ittnotify.h>\n' + text
    text = text.replace(anchor, '''    const auto profile_end = Clock::now() + std::chrono::seconds(180);
    for (unsigned serial = 0; Clock::now() < profile_end; ++serial)''')
    text = text.replace('        auto start_batch = checked(queue->begin());',
                        '        if (serial == 10) { __itt_resume(); }\n        auto start_batch = checked(queue->begin());', 1)
    text = text.replace('cpu[serial - 10]', 'cpu[(serial - 10) % 100]').replace('gpu[serial - 10]', 'gpu[(serial - 10) % 100]')
    text = text.replace('if (serial == 109)', 'if (serial % 100 == 99)')
    stop = text.index('    std::array<std::array<double, 30>, 5> cold{};')
    text = text[:stop] + '    __itt_pause();\n    return 0;\n}\n'
    (output / 'NativeLong.cpp').write_text(text)
    runtime = source / 'modules/function/render/runtime/test/RuntimeBenchmark.cpp'
    if runtime.exists():
        text = '#include <ittnotify.h>\n' + runtime.read_text()
        assert text.count('        frames(264);') == 1
        assert text.count('void finishRuntimeProfile();') == 1
        text = text.replace('void finishRuntimeProfile();', 'void finishRuntimeProfile() {}')
        text = text.replace('        measuring = true;', '        __itt_resume();\n        measuring = true;')
        text = text.replace('        measuring = false;', '        measuring = false;\n        __itt_pause();')
        text = text.replace('        frames(264);', '''        for (unsigned block = 0; block < 90; ++block)
        {
            frames(24 + 240 * (block + 1));
        }''')
        (output / 'RuntimeLong.cpp').write_text(text)
    # Add targets only in an explicit external diagnostic project hook.
    hook = '''function(f6_vtune_targets)
    add_executable(f6_vtune_native "@OUT@/NativeLong.cpp" "@SRC@/modules/function/render/vulkan/test/NativeGraphSupport.cpp")
    target_link_libraries(f6_vtune_native PRIVATE render_vulkan_graph render_vulkan_shader_compiler "@VTUNE@/lib64/libittnotify.lib")
    target_include_directories(f6_vtune_native PRIVATE "@VTUNE@/include"
        "@SRC@/modules/function/render/vulkan/test" "@SRC@/modules/function/render/vulkan/test/shader"
        "${CMAKE_BINARY_DIR}/generated" "${CMAKE_BINARY_DIR}/render_vulkan/test/shader/generated" "${CMAKE_BINARY_DIR}/generated" "@SRC@/modules/core/type_info/include")
    add_dependencies(f6_vtune_native render_vulkan_graph_benchmark)
'''
    if runtime.exists():
        hook += '''    add_executable(f6_vtune_runtime "@OUT@/RuntimeLong.cpp" "@SRC@/modules/function/render/vulkan/test/NativeGraphSupport.cpp")
    target_link_libraries(f6_vtune_runtime PRIVATE render_runtime render_vulkan_shader_compiler "@VTUNE@/lib64/libittnotify.lib")
    target_include_directories(f6_vtune_runtime PRIVATE "@VTUNE@/include"
        "@SRC@/modules/function/render/runtime/test" "@SRC@/modules/function/render/runtime/test/shader"
        "@SRC@/modules/function/render/vulkan/test" "@SRC@/modules/function/render/vulkan/test/shader"
        "${CMAKE_BINARY_DIR}/render_vulkan/test/shader/generated" "${CMAKE_BINARY_DIR}/generated" "@SRC@/modules/core/type_info/include")
    add_dependencies(f6_vtune_runtime render_runtime_benchmark)
'''
    hook += 'endfunction()\ncmake_language(DEFER CALL f6_vtune_targets)\n'
    hook = hook.replace('@OUT@', output.as_posix()).replace('@SRC@', source.as_posix()).replace('@VTUNE@', vtune.as_posix())
    (output / 'profile.cmake').write_text(hook)
    state = {'source_commit': subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip(),
             'original_native': digest(native), 'original_runtime': digest(runtime) if runtime.exists() else None,
             'script_sha256': digest(__file__), 'harnesses': {p.name:digest(p) for p in output.iterdir()}}
    (output/'identity.json').write_text(json.dumps(state,indent=2))


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--vtune',type=Path,required=True)
    args=parser.parse_args()
    prepare(args.source.resolve(),args.output.resolve(),args.vtune.resolve())

