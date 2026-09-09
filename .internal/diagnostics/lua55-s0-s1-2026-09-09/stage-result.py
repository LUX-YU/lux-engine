"""Create the review record after raw evidence is archived; never relabel qualification source."""
import hashlib, json, re, sys
from pathlib import Path
root, evidence = (Path(p).resolve() for p in sys.argv[1:3])
manifest = evidence / 'raw-files.json'
assert manifest.is_file() and (root / 'closing-audit.json').is_file()
candidate = '55dcb3fa85a9a2b7dcc87b178dd9ff76ba75c644'
tests = [dict(configuration=configuration, passed=count, total=count, source=candidate, raw=raw)
    for configuration, count, raw in [
        ('Developer Lua55', 128, 's1-final/d-ctest.xml'),
        ('Developer Lua54', 128, 's1-final/l-ctest.xml'),
        ('Developer LuaJIT', 128, 's1-final-jit/d-ctest.xml'),
        ('Toolchain Lua55 selection', 109, 's1-final/t-ctest.xml')]]
tests += [dict(name='Installed consumers', passed=15, source='2e915205d65acb62505b8a2c026a1976c152c598',
    raw='consumers-lua55/consumers.json'), dict(name='Relocated generated provider and packager',
    passed=2, source=candidate, raw='relocated/results.json'),
    dict(name='Relocated incremental qualifications', passed=13, source=candidate,
         raw='relocated/incremental/probes.json'),
    dict(name='Calibrated before/after OOM, retirement and deferred stop', passed=20,
         driver_source='6cf02a9c9b8efde5624d7ac09341bc004472ab5f', raw='creation-branches-calibrated/')]
result = dict(schema_version=1, stage='S1', status='CORRECTNESS_QUALIFIED_COST_OPEN',
    parent_commit='c72d88ce4c17a5f81f85e278610a3f987d4b828d', candidate_commit=candidate,
    authorized_scope=['S0','S1'], correctness_passed=True, cost_passed=False, installation_passed=True,
    default_vm_switch_approved=False, tested_platforms=['Windows x64 RelWithDebInfo'],
    unverified_platforms=['Linux x64','Android','iOS','console','ASan/UBSan','hardware PMU'],
    raw_manifest_sha256=hashlib.sha256(manifest.read_bytes()).hexdigest(), tests=tests,
    comparisons=json.loads((root/'cost-summary.json').read_text()), rejected_candidates=[],
    invalid_runs=[dict(raw=raw, reason=reason) for raw,reason in [
        ('s0-correctness','Missing freshly cooked read_value artifact; Toolchain Lua oracle not compiled'),
        ('s0-business-smoke','FlowForge linker environment missing; fresh VS run retained separately'),
        ('s0-aa','Stale embedded source label; raw rows unchanged'),
        ('corrected-initial-build','Style failure; subsequent wrong-clone HEAD build retained'),
        ('prefix-oom','Retirement expectation corrected to actual backend return observation'),
        ('creation-branches','Hard-coded registry growth prefix did not establish the intended allocation'),
        ('s1-initial-correctness','Wrong assumption that global is reserved with official default compatibility'),
        ('s1-compatible-install','lux_sdk component alone is not the complete SDK'),
        ('vm-negative','Wrong-DLL full API probe failed earlier at loader, not the assumed version check'),
        ('vm-version-negative','PowerShell no-op check driver argument error'),
        ('costs/safety-jit/cold8-0-a.log','CLI rejects zero warmups; population implementation ignores warmups'),
        ('costs/physics-jit-to55/physics-4-b.log','Unexplained exit 1 with empty log/CSV; only four valid pairs; fresh whole group kept separately'),
        ('physics-empty-exit-diagnostic/','Initial -g skipped exit-breakpoint setup; no exit-probe claim; fixed independent diagnostic retained'),
        ('profile55/','VTune target stdout/stderr absent from collect log; oracle validation unavailable; separate captured run retained')]],
    limits=[
        'Lua54 fixed-P-core A/A failed after the one permitted environment retry; no cost equivalence granted.',
        'Lua55 is substantially slower than LuaJIT in these complete steady workloads; product default switch is not approved.',
        'Same-VM OOM safety correction costs are separate from VM migration. No safety checks were removed.',
        'Existing LuaJIT/Lua54 dependencies were not rebuilt; historical compiler flags are not fully bound to those DLLs.',
        'Cold/sequence/churn/Physics error fields remain unknown where the original harness exports no cumulative counter.',
        'Batch p99 is not per-continuation wall-clock resume latency.',
        'Churn at 8 and 8192 configurations rebuilds 1 and 100 instances per frame respectively; not single-instance scaling.',
        'Memory last-row counters precede oracle/shutdown/VM destruction; no final-live-zero claim.',
        'Software VTune samples cover the whole process; no PMU or direct attribution from historic ROI percentages.',
        'One original Physics timing process exited 1 without a log. Same-work debugger run exited normally; original cause remains unexplained.',
        'Startup capture omitted the first dirty file hash; initial .gitignore content is not retroactively asserted.',
        'No S2-S7/X experiments, pooling, Native ABI, LTO, GC policy, type expansion, main merge or release tag.'
    ], next_allowed_entry='Independent S0/S1 review; no automatic next stage or product VM switch')
schema=json.loads((root/'instructions/LUX_Script_Optimization_Plan_v1/schemas/stage_result.schema.json').read_text())
# Validate the applicable S1 schema fields without adding a Python package dependency.
# The schema's only conditional concerns QUALIFIED_FOR_LISTED_PLATFORMS, which is not this result.
assert set(result)==set(schema['required'])==set(schema['properties'])
assert result['schema_version']==1
for key in ['stage','status']: assert result[key] in schema['properties'][key]['enum']
for key in ['parent_commit','candidate_commit']: assert re.fullmatch('[0-9a-f]{40}',result[key])
assert re.fullmatch('[0-9a-f]{64}',result['raw_manifest_sha256'])
for key in ['correctness_passed','cost_passed','installation_passed','default_vm_switch_approved']:
    assert isinstance(result[key],bool)
for key in ['authorized_scope','tested_platforms','unverified_platforms','limits']:
    assert isinstance(result[key],list) and all(isinstance(item,str) for item in result[key])
for key in ['tests','comparisons','rejected_candidates','invalid_runs']:
    assert isinstance(result[key],list) and all(isinstance(item,dict) for item in result[key])
assert isinstance(result['next_allowed_entry'],str)
assert result['status']=='CORRECTNESS_QUALIFIED_COST_OPEN' and result['cost_passed'] is False
(evidence/'stage_result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2))
print('STAGE_RESULT applicable schema fields valid; correctness/installation passed; COST_OPEN; source',candidate)
