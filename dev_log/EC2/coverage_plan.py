"""Resolve themes to final executed commands, never to probes supplied in the construction package."""
from pathlib import Path
import hashlib,json
w=Path(__file__).resolve().parent;c=json.loads((w/'final-config.json').read_text())
entries=[
('Prepared publication failure/move/self-move/reentrant cleanup keeps one reservation.', ['final-ctest','final-publication-sdk-positive']),
('Workers share immutable plans without Storage release rights; move-only and three actual immutable-input compile negatives.', ['final-ctest','final-contracts']),
('Wrong receipt identity and digest leave manifest/catalog/source unchanged.', ['final-ctest']),
('Actual file publication with lost receipt and manifest failure preserves Unknown and retries without a second successful package write.', ['final-ctest','final-ec2-sdk']),
('Actual SceneConfigurationElement and pure draft share one preparation algorithm; headless SDK has no UI imports.', ['final-ctest','final-ec2-sdk']),
('Missing providers, cycles, unsupported partitions and opaque configurations retain their original rejection/preservation semantics.', ['final-ctest','final-ec2-sdk']),
('GUI, native SDK and loaded plugin registrations use the same public configuration preparation.', ['final-ctest','final-ec2-sdk']),
('Actual builtin artifact and independent foreign IArtifactSource/custom cooked type publish without a window through the original operation.', ['final-ctest','final-external-artifact-test']),
('Source S1 is saved after S0 capture; late S0 artifact publication retains S1 source digest and separate S0 compiled digest.', ['final-ctest','final-ec2-sdk','final-external-artifact-test']),
('Standalone installed ResultsView/WorkspaceView retain prior display on BUSY/IO and emit fixed typed intents; no Application link.', ['final-ctest','final-ec2-sdk']),
('One fixed compilation feeds two actual previews with distinct adoption identity and output resources; stale keys reject.', ['final-ctest','final-sdk']),
('Actual GPU previews use shared recipe generation, second mesh recipe, retained successful resources, close and retirement.', ['final-ctest','final-sdk']),
('Compilation failures and preview/navigation readiness are distinct owning diagnostics in actual services/views.', ['final-ctest','final-sdk']),
('Workspace status does not refresh catalog; real selected-only migration, marker and source-change/conflict regressions retained.', ['final-ctest','final-sdk']),
('ModelImportRecipe pure codec and real ModelImporter initial/reimport/captured-source paths use original cooking/publication.', ['final-ctest','final-ec2-sdk']),
('Installed native capability reads/decodes through original Process without Lua/Editor/GPU/LLVM imports.', ['final-ec2-sdk','final-dll-closure']),
('Actual generated Lua Asset/Skeleton methods execute through original ScriptSystem, package generation and original resume path.', ['final-ctest','final-ec2-sdk','final-player-ctest']),
('Native actual missing/type/decode/IO/limit outcomes; Lua handles NOT_FOUND and continues. Admission and accepted outcomes differ.', ['final-ctest','final-ec2-sdk']),
('Full-width IDs, domain above 2^53 and generation survive opaque Lua value transport; wrong type/table/version and foreign native handles reject.', ['final-ctest','final-ec2-sdk']),
('Capacities 1/2 reject before submission, recover after release, preserve successful and failed completions through backpressure.', ['final-ctest','final-ec2-sdk']),
('Synchronous memory endpoint and blocked/background CPU paths settle only at original completion ingress; no provider callback enters VM.', ['final-ctest','final-ec2-sdk']),
('Stop before IO, before decode, after worker and before delivery/resume drains original tasks and invalidates only the original scope.', ['final-ctest','final-ec2-sdk']),
('Token aliases, duplicate release, slot reuse and cross-instance queries preserve full identity; no implicit ownership by token.', ['final-ctest','final-ec2-sdk']),
('Real ScriptSystem retirement revokes unreleased results before backend/code teardown, releases capacity and does not revive old domains.', ['final-ctest','final-ec2-sdk']),
('Bounded byte copy checks overflow/zero/end/oversize, rejects typed-image confusion; warm queries perform no additional IO.', ['final-ctest','final-ec2-sdk']),
('Asset await followed by original Delay updates Counter through deferred command barrier; Run source bytes/history/binding unchanged.', ['final-ctest','final-player-ctest','final-ec2-sdk']),
('Original provider admission plus new Lua schema/representation/layout failures reject at prepare/call boundaries; empty read port grants no assets.', ['final-ctest','final-sdk']),
('Independent Skeleton ability uses installed generator/normal projection; generic backend has no concrete asset switch.', ['final-ec2-sdk','final-source-audit']),
('Real DLL decoder/nontrivial deleter is pinned through value destruction on the owner thread; native control block fixes actual unload crash.', ['final-ctest']),
('Original LuaBoundary and error/yield regressions plus new rejected values and stop outcomes; no raw nontrivial async transport.', ['final-ctest','final-ec2-sdk']),
('PLAYER profile builds/runs same packaged Lua from pak/VFS, with no Editor source/includes and native result retirement.', ['final-player-build','final-player-ctest','final-dll-closure']),
('The same packaged Lua runs in one actual RunStore instance shown by two installed GPU SceneViews; closing one retains the other, resume completes the script, stop retires slots/resources and leaves full author state unchanged.', ['final-script-views-settled-test','final-ctest','final-sdk']),
('All original test names retained, existing bodies migrated without removing behavior; installed Skeleton HEADLESS/WINDOW/APP remain actual DLL cases.', ['final-ctest','final-skeleton-headless','final-skeleton-window','final-skeleton-app','final-sdk']),
('Actual EC2 boundary positive/illegal/restored fixtures, native import checks, runtime rejection and installed constraint/retired-header negatives.', ['final-ctest','final-ec2-sdk','final-contracts']),
('Fresh SDK generators and standalone C++20 public header parse; no old package/header dependency; changed modules synchronized to three prefixes.', ['final-headers','final-sdk','final-ec2-sdk','final-installed-audit','final-module-sync']),
('Actual counted warm native queries, Lua reads/prepared slots/resumes, capacities and code/result retirement; not an RSS or latency-percentile claim.', ['final-ctest','final-ec2-sdk']),
('Clean all build and no-work; tests include real new/old GPU modes and PLAYER. Native source and SDK input runs are NOT_RUN_USER_DEFERRED: user explicitly selected later, not waived or passed.', ['final-build','final-no-work','final-ctest','final-player-ctest']),
('Single final SHA, original patch/main/history unchanged; archive verifier tests relocation/missing/tampered evidence; inherited waivers remain distinct.', ['final-tracked','final-source-audit','final-installed-audit','final-module-sync'])
]
records={x['name']:x for x in json.loads((w/'commands.json').read_text())};empty=hashlib.sha256(b'').hexdigest()
coverage=[]
for i,(meaning,names) in enumerate(entries,1):
    for name in names:
        r=records[name]
        assert r['exit_code']==0 and r['source_head']==c['implementation_sha'],name
        assert r['source_diff_sha256']==empty and not r['source_untracked_sha256'],name
        assert hashlib.sha256((w/r['log']).read_bytes()).hexdigest()==r['sha256'],name
    coverage.append(dict(id=f'XEC2-{i:02}',status='PARTIAL' if i==37 else 'PASS',meaning=meaning,commands=names))
assert len(coverage)==38
(w/'coverage.json').write_text(json.dumps(coverage,indent=2)+'\n')
print('38 themes: 37 PASS, XEC2-37 PARTIAL (native input user-deferred); inherited scope remains separate')
