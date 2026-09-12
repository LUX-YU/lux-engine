import ctypes as ct
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path

root = Path(__file__).parent
out = root / 'premerge-audit-20260912'
out.mkdir(exist_ok=True)
build = root.parent / 'o/w/d'
source = root / 'local-source'
commands = json.loads((build / 'compile_commands.json').read_text(encoding='utf-8-sig'))
shell = ct.WinDLL('shell32')
shell.CommandLineToArgvW.argtypes = [ct.c_wchar_p, ct.POINTER(ct.c_int)]
shell.CommandLineToArgvW.restype = ct.POINTER(ct.c_wchar_p)
def split(command):
    count = ct.c_int()
    values = shell.CommandLineToArgvW(command, ct.byref(count))
    result = [values[i] for i in range(count.value)]
    ct.windll.kernel32.LocalFree(values)
    return result

def replace_once(text, old, new):
    assert text.count(old) == 1, (old[:80], text.count(old))
    return text.replace(old, new)

timer_path = source / 'engine/domain/simulation/builtin/script/test/script_system_continuation_test.cpp'
timer = timer_path.read_text()
timer = timer[:timer.index('int main()')] + r'''
int main()
{
    bool defect{};
    for (const bool local : {false, true})
    {
        Harness harness{false};
        configureTimerHarness(harness);
        auto& backend = harness.backend_state;
        backend.local_timer = local;
        backend.simulation_timer = true;
        backend.timer_seconds = 9223372036.854776;
        auto system = harness.create(limits(), {});
        assert(system && system->prepare());
        assert(dispatchRuntimeHook(*system, harness.hook) == 1U);
        const auto stats = system->stats();
        const auto status = system->failures().empty() ? 0 : system->failures().front().status;
        std::printf("TIMER_OVERFLOW local=%u input=%.17g long_double_bytes=%zu waits=%zu errors=%llu status=%d expected=%d\n",
            local, backend.timer_seconds, sizeof(long double), stats.simulation_delay_waits,
            stats.invocation_failures, status, static_cast<int>(EScriptDelayStatus::DURATION_OVERFLOW));
        defect |= stats.simulation_delay_waits != 0U || status != static_cast<int>(EScriptDelayStatus::DURATION_OVERFLOW);
        assert(system->shutdown());
        std::printf("TIMER_CLEANUP local=%u creates=%zu destroys=%zu continuations_destroyed=%zu\n",
            local, backend.creates, backend.destroys, backend.continuation_destroys);
    }
    return defect ? 2 : 0;
}
'''

lua_path = source / 'engine/domain/simulation/scripting/lua/test/lua_value_runtime_test.cpp'
lua = "#include <lua.hpp>\n" + lua_path.read_text()
lua = lua[:lua.index('int main(int argc, char** argv)')] + '\nint main() { return resumeAuthorityCase(false); }\n'
lua = lua.replace('static int resumeAuthorityCase(', 'static int resumeAuthorityCase(')  # anchor verified below
lua = replace_once(lua, 'static int resumeAuthorityCase(bool stop)', 'static std::size_t audit_body_calls;\nstatic bool audit_no_retire;\nstatic int resumeAuthorityCase(bool stop)')
lua = replace_once(lua, 'else resume_registry->destroy(resume_entity);', 'else if (!audit_no_retire) resume_registry->destroy(resume_entity);')
lua = replace_once(lua, 'return original.push(state, value);', '''const bool pushed = original.push(state, value);
        if (pushed) {
            lua_pushcfunction(state, +[](lua_State*) -> int { ++audit_body_calls; return 0; });
            lua_setfield(state, -2, "audit");
        }
        return pushed;''')
lua = replace_once(lua, 'description.body = lux::rdesc::LuaSourceScript{"Resume", {kTick}};', 'description.body = lux::rdesc::LuaSourceScript{"Resume"};')
lua = replace_once(lua, 'description.exports = {{"tick", kTick, {}, {}}};', 'description.exports = {{"tick", kTick, {lux::rdesc::makeScriptValueType<ValuePose>(lux::semantic::EValuePass::CONST_REF)}, {}}};')
start = lua.index('    constexpr std::string_view code =', lua.index('static int resumeAuthorityCase'))
end = lua.index('    const auto bytes =', start)
lua = lua[:start] + '    constexpr std::string_view code = "return {tick=function(self,p) p.audit() end}";\n' + lua[end:]
start = lua.index('static int resumeAuthorityCase')
tail = lua[start:]
tail = replace_once(tail, '{{kTick, HookScriptTarget{kOwner, kHook}}}', '{{kTick, EventScriptTarget{kOwner, event_id}}}')
tail = replace_once(tail, '    assert(dispatchRuntimeHook(*system, hook) == 1U);\n    assert(system->activeContinuationCount() == 1U && system->stats().active_event_waiters == 1U);', '')
tail = replace_once(tail, '    assert(deliverRuntimeEvent(*system, event) == 1U);', '')
tail = replace_once(tail, '    const auto stable = executeRuntimeStablePoint(*system);', '    assert(deliverRuntimeEvent(*system, event) == 1U);\n    std::printf("SYNC_CONVERTER_RETIRE conversions=%zu body_calls=%zu entity_valid=%u expected_body_calls=%zu\\n", resume_conversions, audit_body_calls, registry.valid(entity), static_cast<std::size_t>(stop || audit_no_retire));\n    const auto stable = executeRuntimeStablePoint(*system);')
tail = replace_once(tail, 'provider.zero_calls == static_cast<std::size_t>(stop)', 'provider.zero_calls == 0U')
tail = replace_once(tail, '    assert(stable);', '    std::printf("SYNC_STABLE stop=%u control=%u success=%u\\n", stop, audit_no_retire, static_cast<bool>(stable));\n    assert(stop || stable);')
tail = replace_once(tail, '    assert(released.vm_coroutine_creations == 1U && released.vm_coroutine_releases == 1U);', '    assert(released.vm_coroutine_creations == 0U && released.vm_coroutine_releases == 0U);')
tail = tail[:tail.index('    std::printf("RESUME_AUTHORITY')] + '    std::printf("SYNC_RETIRE_CLEANUP prepared_abilities=%zu threads=%llu released=%llu\\n", released.prepared_ability_slots, released.vm_coroutine_creations, released.vm_coroutine_releases);\n    return audit_body_calls == static_cast<std::size_t>(stop || audit_no_retire) ? 0 : 2;\n}\nint main() { audit_no_retire=true; audit_body_calls=0; const int valid=resumeAuthorityCase(false); audit_no_retire=false; audit_body_calls=0; const int stop=resumeAuthorityCase(true); audit_body_calls=0; const int retired=resumeAuthorityCase(false); return valid || stop || retired; }\n'
lua = lua[:start] + tail

observations = []
for name, text, original, target in [
    ('timer-overflow', timer, timer_path, 'simulation_script_continuation_test'),
    ('lua-sync-converter-retire', lua, lua_path, 'simulation_script_lua_value_runtime_test'),
]:
    path = out / (name + '.cpp')
    path.write_text(text, encoding='utf-8')
    entries = [item for item in commands if item['file'].replace('\\', '/').endswith('/' + original.name)]
    assert len(entries) == 1
    entry = entries[0]
    args = split(entry['command'])
    compile_args = [arg for arg in args if not arg.startswith(('/Fo', '/Fd')) and arg.replace('\\', '/') != entry['file'].replace('\\', '/')]
    compile_args += ['/IE:/SyncForder/CodeRepos/install/o/v4/lua55/include/lua55', str(path), '/Fo' + str(out / (name + '.obj')), '/Fd' + str(out / (name + '.compile.pdb')), '/I' + str(original.parent)]
    compile_log = out / (name + '-compile.log')
    with compile_log.open('wb') as log:
        code = subprocess.call(compile_args, cwd=entry['directory'], stdout=log, stderr=subprocess.STDOUT)
    record = {'name': name, 'source_sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'compile_command': compile_args, 'compile_exit': code,
              'production_source': '841320a36e864145eda43fd6b775a10a696d8e7f', 'diagnostic_only': True}
    if code != 0:
        observations.append(record); (out / 'probes.json').write_text(json.dumps(observations, indent=2)); print(name, 'COMPILE_FAILED', flush=True); continue
    # Reuse only the import libraries from this target's existing command; no build or post-build mutation.
    listing = subprocess.check_output(['D:/Softwares/ninja-win/ninja.exe', '-C', str(build), '-t', 'commands', target], text=True)
    link_line = [line for line in listing.splitlines() if ' -E vs_link_exe ' in line][-1]
    link_command = link_line.split(' -- ', 1)[1].split(' && ', 1)[0]
    parsed = split(link_command)
    libraries = [arg for arg in parsed if arg.lower().endswith('.lib') and not arg.startswith('/')]
    libraries += ['E:/SyncForder/CodeRepos/install/o/v4/lua55/lib/lux_lua55.lib']
    link_args = [parsed[0], '/nologo', '/debug', '/INCREMENTAL:NO', '/machine:x64', '/subsystem:console',
                 '/out:' + str(out / (name + '.exe')), '/pdb:' + str(out / (name + '.pdb')), str(out / (name + '.obj')), *libraries]
    with (out / (name + '-link.log')).open('wb') as log:
        link_code = subprocess.call(link_args, cwd=build, stdout=log, stderr=subprocess.STDOUT)
    record.update(link_command=link_args, link_exit=link_code)
    if link_code == 0:
        env = dict(os.environ)
        env['PATH'] = ';'.join([str(root / 'images/E'), 'D:/Development/vcpkg/installed/x64-windows/bin', *[x for x in env['PATH'].split(';') if 'CodeRepos' not in x and 'vcpkg' not in x]])
        with (out / (name + '.log')).open('wb') as log:
            run = subprocess.call([str(out / (name + '.exe'))], env=env, stdout=log, stderr=subprocess.STDOUT)
        record.update(run_exit=run, output=(out / (name + '.log')).read_text(errors='replace'), exe_sha256=hashlib.sha256((out / (name + '.exe')).read_bytes()).hexdigest())
        print(name, run, record['output'], flush=True)
    else:
        print(name, 'LINK_FAILED', flush=True)
    observations.append(record)
    (out / 'probes.json').write_text(json.dumps(observations, indent=2))
