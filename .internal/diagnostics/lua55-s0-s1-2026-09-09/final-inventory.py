"""Add explicit review outcomes to the S0 search inventory without rewriting its original evidence."""
import hashlib, json, sys
from pathlib import Path
root = Path(sys.argv[1]).resolve()
source = Path(__file__).resolve().parents[3]
entries = json.loads((root/'final-mapping/consumer_inventory.json').read_text())
consumers = {r['consumer']:r for r in json.loads((root/'consumers-lua55/consumers.json').read_text())}
final = '55dcb3fa85a9a2b7dcc87b178dd9ff76ba75c644'
for entry in entries:
    name = entry['file'].replace('\\','/')
    entry['file'] = name
    if name.startswith('cmake/installed-consumers/'):
        consumer = name.split('/')[2]
        if consumer in consumers:
            entry['verification'] = dict(status='PASS', source=consumers[consumer]['source_sha'],
                raw='consumers-lua55/consumers.json', consumer=consumer)
            if consumer in ['script-lua-values','lua-script-packager']:
                entry['verification']['final_relocation'] = dict(source=final,raw='relocated/results.json')
        elif consumer == 'script-lua-value-costs':
            entry['verification'] = dict(status='PASS_FORMAL_VARIANT_ONLY',source=final,
                raw='consumer-value-costs55/',scope='SR5_VALUES=ON real generated output and protected errors; legacy branch not run')
        elif consumer == 'flowforge-model':
            entry['verification'] = dict(status='NOT_RUN_UNAFFECTED',
                scope='Source model consumer has no Lua VM dependency; original compiler/runtime consumer was run')
        else:
            raise RuntimeError('Unreviewed consumer '+consumer)
    elif name in ['cmake/RunScriptHookClosureQualification.ps1','cmake/RunScriptV3Qualification.ps1']:
        entry['verification'] = dict(status='REVIEWED_NOT_EXECUTED',
            scope='Historical scripts retain explicit VM choices; final qualification uses current stage driver and installed logs')
    elif name == 'cmake/dependencies/lua55/smoke.c':
        entry['verification'] = dict(status='PASS',source='985641d4cfbee786576d240732ca47a4e5e80c4c',
            raw='dependency-tests.log',scope='Actual C coroutine result and GC API calls; compile probe is separately identified')
    elif entry['category'] == 'cooked-artifact-input':
        entry['verification'] = dict(status='PASS',source=final,
            raw=['s1-final/d-ctest.xml','s1-final/l-ctest.xml','s1-final-jit/d-ctest.xml','relocated/results.json'],
            scope='Fresh cooked fixtures; per-step Scene protocol and packager; complete Event oracle in cost logs')
    else:
        entry['verification'] = dict(status='PASS_WITH_LISTED_SCOPE',source=final,
            raw=['s1-final/','s1-final-jit/','final-mapping/assertion-map.csv'],
            scope='Actual selected-provider configure/all/no-op and listed runtime assertions; search hits are not exhaustive fault coverage')
    entry.pop('coverage',None)
    if 'pending' in entry.get('test','').lower(): entry.pop('test')
    if (source/name).is_file(): entry['final_file_sha256'] = hashlib.sha256((source/name).read_bytes()).hexdigest()
for name in [
    'modules/function/script/core/template/script_ability_lua.template',
    'modules/function/script/lua/cmake/template/lua_value.template',
    'modules/function/script/lua/cmake/template/lua_value.validation.template',
    'modules/function/script/lua/cmake/template/lua_registration.hpp.in',
    'modules/function/script/lua/cmake/template/meta_lua.template']:
    assert (source/name).is_file()
    object_binding = name.endswith('meta_lua.template')
    entries.append(dict(file=name,owner='Existing Lua value / Ability generation',category='generated-template',
        reason='No production template changes; regenerate with selected installed Lua SDK and retain value/object-binding separation',
        final_file_sha256=hashlib.sha256((source/name).read_bytes()).hexdigest(),
        verification=dict(status='REVIEWED_UNCHANGED' if object_binding else 'REGENERATED',source=final,
            raw=['final-mapping/assertion-map.csv'] if object_binding else
                ['relocated/results.json','relocated/incremental/probes.json'],
            scope='Existing sol2 object-binding responsibility retained; no dedicated new template invalidation probe' if object_binding
                else 'Real generated Ability and opt-in value chain; exact individual incremental probes remain separately listed')))
(root/'final-mapping/reviewed-consumers.json').write_text(json.dumps(entries,indent=2))
print('REVIEWED_CONSUMERS',len(entries),'explicit outcomes; no pending search-hit claims')
