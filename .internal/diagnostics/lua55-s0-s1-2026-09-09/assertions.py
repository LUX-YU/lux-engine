"""Source assertions and actual CTest results, not name-similarity coverage."""
import csv, json, re, sys, xml.etree.ElementTree as ET
from pathlib import Path
root=Path(sys.argv[1]).resolve()
source=Path(__file__).resolve().parents[3]
out=root/'final-mapping';out.mkdir(exist_ok=True)
candidate='55dcb3fa85a9a2b7dcc87b178dd9ff76ba75c644'
parent='c72d88ce4c17a5f81f85e278610a3f987d4b828d'
logs=['s1-final/d-ctest.xml','s1-final/l-ctest.xml','s1-final-jit/d-ctest.xml']
suites={log:{t.attrib['name']:t for t in ET.parse(root/log).getroot().findall('testcase')} for log in logs}
rows=[]
def add(id,path,symbol,test,assertions,scope='Complete listed assertion; no broader coverage inferred'):
    text=(source/path).read_text()
    assert symbol in text,(path,symbol)
    line=next(n for n,s in enumerate(text.splitlines(),1) if symbol in s)
    for log in logs:
        actual=suites[log].get(test)
        passed=actual is not None and actual.find('failure') is None and actual.find('skipped') is None
        result='PASS' if passed else 'NOT_RUN'
        if id=='T-L12' and 'LUA55_LANGUAGE' not in (actual.findtext('system-out','') if actual is not None else ''):
            result='VERSION_NOT_APPLICABLE'
        rows.append(dict(test_id=id,origin='Retained or narrow S1 addition',source_path=path,symbol=symbol,
            source_line=line,ctest_name=test,parent_sha=parent,candidate_sha=candidate,log_path=log,
            result=result,assertions=assertions,scope=scope))
base='engine/domain/simulation/builtin/script/test/'
old={
'lifecycle':[
('testInitialLifecycle','3 creates precede first BeginPlay; begins=ends=destroys=3; prepared releases=prepares'),
('testOptionalAndFailureLifecycle','Failed BeginPlay: creates=3; qualified BeginPlay/EndPlay=1; destroys=3'),
('testIncarnationAndPendingContinuation','Old entity two continuation destroys; four begins total; old completion INVALID_ID; resumes=0')],
'runtime':[('testPrepareRollbackBusy','Busy prepare rollback retains protected resources and retries cleanup')],
'continuation':[
('testSyncAndContinuation','Sync calls=1/pending=0; early completion resumes once then second; no recursive resume depth'),
('testAsyncAbilityInvocation','Exact asynchronous provider/result association and completed business count'),
('testCapacityAndCancellation','Capacity errors preserved; cancelled continuations destroyed; no duplicate completion business')],
'event_wait':[
('testRegistrationCutoff','Callback registration excluded from current occurrence and admitted on following occurrence'),
('testNestedDispatch','Nested occurrence and claimed waiter order retained'),
('testResumeBudget','Budget one requires successive stable points; no extra resume opportunity'),
('testOutputSensitiveRetirement','Retirement visits only owned records; unrelated waiters survive'),
('testPreparedAdmissionProvenance','Other runtime and old incarnation UNDECLARED_SOURCE; no awaitable admitted'),
('testCopyRetirementPin','During copy pin=1/deferred=1/active=0; after copy zero; no resume; destroy once'),
('testCopyOtherRecordRemoval','Payload 73 survives other removal; two route lookups/eight bytes/two destroys'),
('testCopyShutdownAndFailure','Shutdown ENDPOINT_BUSY; copy failure does not queue; stop completion drains during shutdown'),
('testCopyNestedAdmission','Payloads 81/82/82/82; claimed capacity pressure WAITER_CAPACITY_EXCEEDED')]
}
for family,entries in old.items():
    for symbol,assertion in entries:
        add('OLD-'+symbol,base+'script_system_'+family+'_test.cpp',symbol,
            'simulation_script_'+family+'_test',assertion)
lua='engine/domain/simulation/scripting/lua/test/'
vm='simulation_script_lua_vm_coroutine_contract_test'
boundary='simulation_script_lua_value_boundary_test'
add('T-L01',lua+'lua_vm_coroutine_contract_test.cpp','int main',vm,
    'YIELD/OK result counts; separately dependency smoke yields then returns 42 and closes')
add('T-L02',lua+'lua_vm_coroutine_contract_test.cpp','int main',vm,
    'Three suspensions, C arguments 11/12, resumed 8/9, final 51/22/33; no stack accumulation')
add('T-L03',lua+'lua_vm_coroutine_contract_test.cpp','THREAD_REUSE_REJECTED',vm,
    'Retained old thread remains dead/reachable by closure; fresh thread distinct; cancellation/error identity retained')
for suffix in ['oom_0','oom_1','registry_oom','retire_test','stop_test']:
    add('T-L05-'+suffix,lua+'lua_coroutine_integration_test.cpp','testCreation',
        'simulation_script_lua_creation_'+suffix,
        'OOM -10; original stack; failed provider=0; reused slot; roots=releases; retire -1; deferred stop valid then cleanup')
add('T-L06', 'modules/function/script/lua/test/lua_value_test.cpp','CUSTOM_READ_FIELD_ORDER',boundary,
    'Custom first reader changes later 2 to 73; typed result reads 73; first=29; mutations=1; live returns zero')
add('T-L07',lua+'lua_value_runtime_test.cpp','admissionCase','simulation_script_lua_value_runtime_test',
    'Real retire/fault scalar and zero-argument provider=0; deferred stop current call valid; input recovery valid',
    'Includes same-VM nested other instance; dedicated retire-other-during-conversion combination not separately added')
for id,symbol in [('T-L08','testAbilityProvenance'),('T-L10','testEventProvenance'),('T-L20','DEPTH_RECOVERY')]:
    add(id,lua+'lua_closure_provenance_test.cpp',symbol,'simulation_script_lua_closure_provenance_test',
        'Closure/prototype/instance provenance retained; nested depth 4 providers=30/rejections=16 per run, twice; -8 only')
add('T-L11','modules/function/script/lua/test/lua_value_test.cpp','testNumericContract',boundary,
    'i32/u32 bounds; supported f64 2^53 neighborhood; i64/u64 codecs still unsupported; finite and negative zero')
add('T-L12',lua+'lua_vm_coroutine_contract_test.cpp','LUA55_LANGUAGE',vm,
    'Lua55 official compat_global=1 accepts global; for variable readonly; float roundtrip; no wire change',
    'Lua55-only preprocessor branch; JIT/54 run retained common VM assertions, not Lua55 syntax branch')
add('T-L19',lua+'lua_coroutine_integration_test.cpp','int main','simulation_script_lua_coroutine_integration_test',
    'Existing async/cancel/timer/event and capacity/cleanup assertions unchanged; no new close policy')
with (out/'assertion-map.csv').open('w',newline='') as stream:
    writer=csv.DictWriter(stream,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
(out/'not-run.json').write_text(json.dumps(dict(
    later_stage_only=['T-L04','T-L09','T-L14','T-L15','T-L16','T-L17','T-L18'],
    platform_not_run=['Linux x64','Android','iOS','console','ASan/UBSan','PMU'],
    T_L13='See vm-negative/results.json (SDK mismatch); vm-version-negative-fixed/results.json (runtime version)',
    caveat='A passing executable is not evidence that a version-excluded branch ran; T-L12 Lua55 branch only'),indent=2))
print('assertions',len(rows),'rows',sum(r['result']=='PASS' for r in rows),'PASS')
