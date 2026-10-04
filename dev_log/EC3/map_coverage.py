"""Map evidence to EC3 topics without treating a test count as qualification."""
from pathlib import Path
import json, re

w = Path(__file__).resolve().parent
p = w.parent / 'migration-ledger.json'
ledger = json.loads(p.read_text())
ec3 = ledger['ec3']
commands = {v['name']: v for v in json.loads((w / 'commands.json').read_text())}
full = (w / 'logs/final-ctest.log').read_text(errors='replace')
passed = set(re.findall(r'Test\s+#\d+:\s+(\S+)\s+\.+\s+Passed', full))
# These are concrete evidence routes. Earlier results retain their original SHA/diff
# binding and are not promoted to final-SHA SDK/whole-product qualification.
routes = {
 'V01': ('baseline.json', [], 'Protected workspace hash and inherited scope; no history rewritten.'),
 'V02': ('target-providers.json', [], 'Actual provider graph and per-declaration source blobs; final SDK ABI closure pending.'),
 'V03': ('c1-sdk-run', ['editor.commands'], 'Final installed declaration consumer pending.'),
 'V04': ('c3-provider-final-regression', ['editor.command_index'], 'Controlled same-algorithm collision, forged identity and domain cases.'),
 'V05': ('c3-provider-final-regression', ['editor.command_index'], 'Changed-entry CURRENT/PINNED identity and callback counts.'),
 'V06': ('c8-measure-sdk-runtime', ['editor.command_index'], 'Count scope excludes caller-owned token construction and DLL allocations.'),
 'V07': ('c4-settings-guard-regression', ['editor.contributions.r11-reflection','editor.contributions.r11-cleanup','editor.contributions.r11-notify'], 'Original participating-owner guards retained; final external SDK repetition pending.'),
 'V08': ('c1-sdk-run', ['editor.commands','editor.command_index'], 'External text remains a cold validation boundary.'),
 'V09': ('c8-measure-sdk-runtime', ['editor.commands'], 'Static text zero-copy is not an allocation-free owner claim.'),
 'V10': ('c1-sdk-run', ['editor.commands'], 'Dynamic backing and original code ownership; final SDK repetition pending.'),
 'V11': ('c4-sdk-skeleton-headless', ['editor.contributions'], 'Real external DLL result belongs to its recorded earlier implementation; final SDK pending.'),
 'V12': ('c3-menu-lifetime-regression', ['ui.root'], 'Real CommandMenu and immutable entry lifetime.'),
 'V13': ('c3-extracted-sdk-complete-negative-9-build', [], 'Earlier nine declaration negatives retained; final prefix negative matrix pending.'),
 'V14': ('c3-provider-final-regression', ['editor.commands'], 'Original 50-declaration scope plus dynamic families in sole ledger; final installed catalog pending.'),
 'V15': ('c3-scene-commands-owner-regression', ['editor.scene_execution.controls'], 'Real actions retain original Runtime/RunStore; final application repetition pending.'),
 'V16': ('c2-sdk-consumed-role', [], 'Independent IO and direct/menu wiring recorded; final complete three-entry application qualification pending.'),
 'V17': ('c5-shortcut-focused-root-regression', ['ui.root','editor.commands'], 'Single parser and stable numeric menu route.'),
 'V18': ('c4-sdk-skeleton-window', [], 'Actual external factory WINDOW run recorded before final implementation; final SDK pending.'),
 'V19': ('c2-sdk-project-saving', [], 'Real no-Application SDK source saving; final SDK pending.'),
 'V20': ('c2-sdk-project-saving', [], 'Source/manifest Unknown and partial completion remain separate; final SDK pending.'),
 'V21': ('c8-drop-output-readiness', [], 'Application session-deduplicated save/close regression; final run blocked.'),
 'V22': ('c2-sdk-workspace-busy', [], 'Actual file/Host workspace decisions, including BUSY preservation; final SDK pending.'),
 'V23': ('c2-sdk-consumed-role', [], 'Independent recovery and consumed source role; final SDK pending.'),
 'V24': ('c2-sdk-recent-activity', ['editor.plugin_publication'], 'Recent user file and project manifest remain distinct owners.'),
 'V25': ('c8-drop-output-readiness', ['editor.commands'], 'Application regression plus original dispatch admission; final application blocked.'),
 'V26': ('c4-settings-value-regression', [], 'Five scopes with per-entry allowed scope; final settings test not reached.'),
 'V27': ('c5-font-restart-regression', [], 'Actual startup reads before window/plugin page creation; final startup blocked.'),
 'V28': ('c5-settings-content-regression', [], 'Unknown values retained and corrupt/permission errors visible; final settings pending.'),
 'V29': ('c5-font-restart-regression', [], 'Read-only legacy profile migration preserves opaque/recovery/marker, final repetition pending.'),
 'V30': ('c5-settings-busy-regression', [], 'Draft origin conflicts and Revert; final settings pending.'),
 'V31': ('c5-profile-completions-regression', [], 'This command contains an earlier failure, not PASS; later c5-font-restart-regression is the corrected path. Final IO pending.'),
 'V32': ('c4-sdk-skeleton-window', [], 'Real V9 dynamic settings DLL/page; final SDK pending.'),
 'V33': ('c5-settings-report-regression', [], 'Applied/persisted/restart facts are tested independently; final UI pending.'),
 'V34': ('c5-font-restart-regression', [], 'Declared restart policy, actual selected font and scale; not a manual all-font/DPI qualification.'),
 'V35': ('c5-shortcut-focused-root-regression', ['ui.root'], 'Overrides applied via original parser at change boundary; final persistent/SDK run pending.'),
 'V36': ('c5-window-insets-desktop', ['window.placement'], 'Synthetic placement and actual Windows creation are distinct evidence.'),
 'V37': ('c5-window-pure-regression', ['window.placement'], 'Negative monitor coordinates/disconnect/small work area are controlled model tests, not multi-monitor hardware claims.'),
 'V38': ('c5-window-insets-desktop', ['window.placement','editor.scene_views_gpu'], 'Logical units versus framebuffer and sampled viewport extent; final window tests pending.'),
 'V39': ('c5-window-desktop-regression', ['window.placement'], 'Actual backend modes and normal-rectangle retention; final window tests pending.'),
 'V40': ('c5-font-restart-regression', [], 'Explicit launch/offscreen overrides do not auto-persist; final repetition pending.'),
 'V41': ('c8-camera-patch-gpu', ['editor.scene_views_gpu'], 'Original ECS patch/extraction/output chain, no Editor-to-private-backend shortcut.'),
 'V42': ('c8-camera-patch-gpu', ['editor.scene_views_gpu'], 'Actual final dual viewport GPU passed before quarantine; broader final SDK GPU matrix pending.'),
 'V43': ('c6-camera-gpu-after', [], 'Borrowed camera regression is earlier evidence; final two explicit presentation modes blocked.'),
 'V44': ('c6-camera-gpu-after', [], 'Before-output failure retained; corrected delayed-output ray/adoption test awaits final presentation mode.'),
 'V45': ('c6-camera-gpu-after', ['scene.render_resources'], 'Original observers and initial folding retained; final presentation matrix incomplete.'),
 'V46': ('c8-camera-patch-gpu', ['scene.runtime','editor.scene_views_gpu'], 'Original invalid generation and retirement checks; final presentation SDK pending.'),
 'V47': ('c8-camera-patch-gpu', [], 'Per-function audit plus 1000 no-op ECS counter test; final presentation modes blocked.'),
 'V48': ('c7-array-and-fields', ['editor.inspector_codegen'], 'Actual generated field consumer was run earlier; final installed field consumer pending.'),
 'V49': ('c7-annotation-regression', ['editor.inspector_codegen'], 'Actual host validation, exact integer bounds and invalid annotations.'),
 'V50': ('c7-real-fields', ['editor.scene.editing'], 'Generated author consumer executed earlier. Generated Run control gesture extension prepared but NOT_RUN; no claim based only on manual field calls.'),
 'V51': ('c8-sdk-publication-qualified', [], 'Actual earlier installed incremental events retained; final prefix run pending.'),
 'V52': ('c8-sdk-publication-qualified', [], 'Actual staging/formatter/output failure tests; old failed fixture records retained.'),
 'V53': ('c8-sdk-publication-qualified', [], 'Earlier installed same-stem/Chinese-space generation; final SDK blocked.'),
 'V54': ('c7-array-and-fields', ['scene.script_asset_plugin','scene.script_asset_lua'], 'Original Script/Render projections built in final all; final external consumers pending.'),
 'V55': ('removed-symbols.json', ['editor.inspector_codegen'], 'Production Python emitter and old owner deleted; test driver remains. Final installed deletion scan pending.'),
 'V56': ('c8-measure-sdk-runtime', [], 'Raw M1-M3 counters retained; Descriptor 104/Entry 136/Handle 16, dispatch is not zero-allocation. Final SDK timing repetition pending.'),
 'V57': ('c8-format-token-check.json', ['editor.command_index'], 'Token-preserving formatting and per-check lifetime map; semantic checks are separate.'),
 'V58': ('removed-symbols.json', [], 'Actual provider graph/removal scans; final-independent-boundaries in progress; final install scan pending.'),
 'V59': ('final-build', [], 'Clean tracked EC3+STRICT all and no-work passed; complete CTest interrupted by Defender quarantine. PLAYER/SDK/installed headers NOT_RUN.'),
 'V60': ('baseline.json', [], 'User patch untouched/unapplied, final issue mapping and frozen acceptance not yet completed.')
}
for topic in ec3['coverage']:
    name, tests, scope = routes[topic['id']]
    command = commands.get(name)
    topic['execution'] = {
        'status': 'QUALIFICATION_INCOMPLETE' if topic['id'] != 'V01' else 'BASELINE_PRESERVED',
        'evidence_path': command['log'] if command else name,
        'command_name': name if command else None,
        'command_exit_code': command['exit_code'] if command else None,
        'command_source_head': command['source_head'] if command else None,
        'source_diff_sha256': command['source_diff_sha256'] if command else None,
        'final_tests': [{'name': t, 'result': 'PASS' if t in passed else 'NOT_OBSERVED'} for t in tests],
        'scope': scope,
    }
for issue in ec3['issues']:
    issue['coverage_topics'] = issue['validation'].split()
    if issue['status'] != 'BASELINE_RECORDED':
        issue['status'] = 'IMPLEMENTED_QUALIFICATION_INCOMPLETE'
    issue['qualification_rule'] = 'Per-topic source/SDK/IO/GPU evidence, not batch or test-count inference.'
ec3['next_entry'] = 'Resolve recorded Defender quarantine; resume final qualification without replacing earlier failures.'
p.write_text(json.dumps(ledger, ensure_ascii=False, indent=2) + '\n')
print('Mapped', len(routes), 'topics and', len(ec3['issues']), 'issues; no blanket PASS assigned.')
