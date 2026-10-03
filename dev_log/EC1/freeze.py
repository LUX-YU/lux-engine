"""Freeze the one mutable EC1 record after final qualification; never alter historical archives."""
from pathlib import Path
import hashlib, json, shutil, subprocess
from datetime import datetime, timezone

w = Path(__file__).resolve().parent
source = Path('E:/SyncForder/CodeRepos/lux-engine-p12')
clean = Path('E:/SyncForder/CodeRepos/lux-engine-ec1')
base = '54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8'
sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip()
assert sha == subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=clean, text=True).strip()
assert not subprocess.check_output(['git', 'status', '--porcelain'], cwd=source).strip()
records = json.loads((w/'commands.json').read_text())
by_name = {c['name']: c for c in records}
required = ['final-tracked','final-configure','final-build','final-no-work','final-ctest','final-install',
            'final-player-build','final-player-no-work','final-player-ctest','final-headers','final-sdk',
            'final-skeleton-headless','final-skeleton-window','final-skeleton-app','final-source-audit',
            'final-native-input','final-native-sdk','final-installed-audit','final-module-sync']
empty = hashlib.sha256(b'').hexdigest()
for name in required:
    c = by_name[name]
    assert c['exit_code'] == 0 and c['source_head'] == sha, name
    assert c['source_diff_sha256'] == empty and not c['source_untracked_sha256'], name
    assert hashlib.sha256((w/c['log']).read_bytes()).hexdigest() == c['sha256'], name

meanings = [
 'EditExecutor contains the actual unique algorithm; old EditHistory mutation API and production calls absent.',
 'Original history/model cases retain NO_CHANGE, budgets, failure source/log/redo, undo and redo assertions.',
 'History gate remains on its data owner: two Executors reentering one history and non-owner thread are rejected.',
 'P01-P11 R1/R2 input cleanup, completion, retired state and code-pinning regressions; real Skeleton DLL unload order.',
 'Installed HEADLESS uses ProjectStorage fixed VFS read and catalog, original Skeleton codec; no application/UI target.',
 'Installed WINDOW constructs actual Pane/Element and DetachedView with real Root/ViewHost focus and close.',
 'V8 declared Session/Project/Workbench capabilities; builtins use the same factories; host contains no Skeleton branch.',
 'Manifest v3 open AssetTypeId/source version; v1/v2 read migration and legal unknown-provider data preserved.',
 'Session/View factory indexes select exact kinds; duplicate default is ambiguous and wrong capabilities reject.',
 'Actual builtin content operations and application; Skeleton open/save/SaveAs/reload/close/recovery through same route.',
 'Legally published content survives missing view provider; failure reports display scope rather than rollback.',
 'ViewContent supports zero/one/multiple session references and optional primary; shared two-view Skeleton content.',
 'Real two-bone data, root name/global transform, preserved indices/bind matrices, original codec and actual IO.',
 'Actual DLL code pins cover tasks/sessions/views; removed provider preserves files/catalog and builtin Material remains usable.',
 'AuthoringFacts/application cases cover 2D/3D/mixed/no hierarchy and partition contracts; final domain validation preserved.',
 'Installed-but-undeclared differs from unknown stored data; unknown payload preservation regressions retained.',
 'Feature choices consult author facts while original dependency/conflict/device checks remain authoritative.',
 'Fixed source stamps/revisions and explicit targets retained across UI input, callback and asynchronous adoption.',
 'ProjectCatalogSnapshot owner/view relationship private; copied observations stay same version with indexed lookup.',
 'ResultIntent/WorkspaceIntent alternatives carry matching typed payloads; original legal product actions retained.',
 'Outliner uses complete identities and one membership set; ModelCreation uses captured snapshot.find per asset.',
 'Short real snapshot/projection measurements plus final cost regressions; full projection rebuild explicitly retained.',
 'Clean tracked all build, STRICT actual forbidden-dependency fixtures, independent installed headers and generator output.',
 'One final implementation SHA for qualification, Git file hashes, archive verification and explicit inherited waivers.'
]
extra = {
 1:['final-source-audit'], 4:['final-skeleton-headless','final-skeleton-window'],
 5:['final-skeleton-headless'], 6:['final-skeleton-window'],
 7:['final-source-audit','final-skeleton-app'], 9:['final-skeleton-headless'],
 10:['final-skeleton-app'], 11:['final-skeleton-headless'],
 12:['final-skeleton-app'], 13:['final-skeleton-headless','final-skeleton-app'],
 14:['final-skeleton-headless','final-skeleton-window','final-skeleton-app'],
 21:['final-source-audit'], 22:['final-source-audit'],
 23:['final-headers','final-sdk','final-player-ctest','final-build','final-no-work','final-module-sync'],
 24:['final-tracked','final-source-audit','final-sdk','final-native-input','final-native-sdk']
}
coverage = [dict(id=f'XEC-{i:02}',status='PASS',meaning=meaning,
                 commands=['final-ctest',*extra.get(i,[])]) for i,meaning in enumerate(meanings,1)]
(w/'coverage.json').write_text(json.dumps(coverage,indent=2)+'\n')
receipt = dict(phase='EC1',migration_stage='EC1',layering_mode='STRICT',stop_after='EC1',status='PASS',
               input_sha=base,implementation_sha=sha,user_patch_applied=False,
               frozen_at=datetime.now(timezone.utc).isoformat(),
               checkout=str(source),qualification_checkout=str(clean),
               inherited=dict(P12='PARTIAL_USER_WAIVER',linux='NOT_RUN',system_ime='NOT_RUN',
                              old_50k='PARTIAL_NO_MORE_SAMPLES'),
               scope='EC1 S0-S7 Windows and installed external Skeleton DLL; no main merge or release')
(w/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
ledger_path=w.parent/'migration-ledger.json'
ledger=json.loads(ledger_path.read_text(encoding='utf-8-sig'))
ec=ledger['ec1']; ec['status']='COMPLETE_WAITING_REVIEW'; ec['implementation_sha']=sha
ec['stages']={f'S{i}':'COMPLETE_FINAL_QUALIFIED' for i in range(8)}
ec['final']=dict(stage='EC1',mode='STRICT',evidence='dev_log/EC1/',qualified_commands=required,
                 linux='NOT_RUN',ime='NOT_RUN',user_patch_applied=False)
for key in ['s1','s2','s3','s4','s5','s6']:
    ec[key]['final_qualification']='PASS_FINAL_'+sha
    for old in ['final','sdk_final']:
        if old in ec[key]:ec[key][old]='PASS_FINAL_'+sha
ledger_path.write_text(json.dumps(ledger,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
(w/'ledger-snapshot.json').write_bytes(ledger_path.read_bytes())

# Staging remains inside the one mutable construction node until frozen into Git.
archive=w/'archive'; assert not archive.exists(), 'Do not overwrite an archive'
archive.mkdir()
(archive/'.gitattributes').write_text('* -text\n')
package=Path('C:/Users/ChenHui/Downloads/LUX_ENGINE_EC1_CONVERGENCE_2026-10-02.zip')
assert hashlib.sha256(package.read_bytes()).hexdigest()==ec['package_sha256']
shutil.copy2(package,archive/package.name)
names=['commands.json','coverage.json','receipt.json','baseline.json','files.json','file-actions.json',
       'protection.json','source-audit.json','installed-audit.json','ledger-snapshot.json','module-header-sync.json',
       'final-dependency-seed.json','sdk-dependency-seed.json','report.md','S0.md','S2.md','S5-notes.md',
       'run.py','qualify.py','headers.py','audit.py','installed_audit.py','module_sync_check.py',
       'file_actions.py','harvest.py','freeze.py','portable.py']
for name in names:shutil.copy2(w/name,archive/name)
for path in w.glob('final-*.log'):shutil.copy2(path,archive/path.name)
for path in w.glob('final-*.txt'):shutil.copy2(path,archive/path.name)
for name in ['input','logs','public-headers','sdk','build-proof']:
    shutil.copytree(w/name,archive/name)
artifacts=[dict(path=p.relative_to(archive).as_posix(),sha256=hashlib.sha256(p.read_bytes()).hexdigest())
           for p in sorted(archive.rglob('*')) if p.is_file()]
(archive/'artifacts.json').write_text(json.dumps(artifacts,indent=2)+'\n')
print('staged EC1 archive',sha,len(artifacts),'files')
