from pathlib import Path
import json, subprocess
w = Path(__file__).resolve().parent
p = w.parent / 'migration-ledger.json'
data = json.loads(p.read_text())
ec3 = data['ec3']
ec3['batches']['C2'] = {
    'status': 'IN_PROGRESS',
    'completed_closure': 'ProjectContentSaving and ProjectPluginSelection; true SDK without Application/Root',
    'remaining': ['RecentProjects integration and qualification', 'WorkspaceActions and migration ownership',
                  'independent workbench recovery orchestration', 'C2 final install/dependency qualification']
}
ec3['c2_project_activities'] = {
    'implementation_sha': None,
    'unique_owners': {
        'source encoding/adoption': 'existing SaveService',
        'publication lanes/disk outcomes': 'existing WriteCoordinator',
        'source + catalog association and acknowledgement': 'ProjectContentSaving',
        'fixed SaveAll traversal': 'existing SaveAllOperation with synchronous borrowed admission callback',
        'project plugin desired manifest publication': 'ProjectPluginSelection using original ProjectPublicationOperation',
        'manifest/current catalog': 'existing ProjectStorage',
        'UI review/close permits': 'existing Application/ViewHost/CloseSessionsOperation, borrowing accepted SaveIds'
    },
    'removed_bodies': ['EditorApplication::Impl::prepareSave', 'rememberSave', 'settleSaves',
                       'Application execute/menu AcceptedOperation.kind==save special cases',
                       'Application plugin baseline comparison and publication construction'],
    'removed_state': ['Impl::SavePresentation', 'Impl::PreparedSave', 'Impl::pending_saves_',
                      'Impl::save_reports_', 'Impl::save_all_', 'Impl::plugin_publication_'],
    'check_proofs': [
        'New SaveId uniqueness is established by SaveService admission; no callback intervenes before activity pending insertion.',
        'track() owns idempotence for Close fixed-set visits; callers no longer duplicate pending membership checks.',
        'The second track loop immediately before close was redundant: all save IDs are already tracked and terminal reports checked; no intervening mutation invalidates that fact.',
        'Explicit based_on is rechecked by SaveService after extensible describe; physical destination resolution cannot silently rebase a retained command source.'
    ],
    'runs': ['c2-saving-build', 'c2-saving-regression', 'c2-owner-completion-build',
             'c2-owner-completion-regression', 'c2-sdk-project-saving', 'c2-project-activities-build',
             'c2-project-activities-tests', 'c2-plugin-activity-install', 'c2-sdk-plugin-selection'],
    'retained_failures': {
        'c2-sdk-before-unknown': 'Fixture compared display filename to canonical lowercase physical key; fixture corrected to resolve actual key.',
        'c2-sdk-unknown-reproduction': 'Real source-published/catalog-failed recovery rejected extended Windows physical binding; corrected physical-domain relative path.',
        'c2-sdk-source-unknown-before': 'Real premature acknowledgement at source Unknown; activity now leaves original outcome/lane with SaveService until reconciliation.',
        'c2-sdk-owner-completion': 'Fixture expected post-Unknown baseline adoption; original contract intentionally abandons adoption. Strengthened to require conflict and unchanged bytes before explicit Save As recovery.',
        'c2-sdk-unknown-baseline-build': 'Diagnostic-only test referenced nonexistent reason member; corrected to actual code.'
    },
    'sdk_behaviors': [
        'Scene/Material/Flow real save, SaveAs, decode, checkpoint; no App/Root linkage',
        'Export preserves checkpoint/history; explicit stale source rejected after resolver callback edits model',
        'Source publication/catalog failure, physical-binding recovery; manifest and source Unknown retention',
        'Reconciliation does not restore abandoned adoption; subsequent conflict retained; SaveAs then viewless SaveAll succeeds',
        'Plugin stale draft/BUSY/wrong-thread; real manifest publication, lost receipt/reconcile, explicit abandon and acknowledgement'
    ],
    'scope': 'Bounded C2 closure only; not EC3 final acceptance, no deferred native input or unrelated long benchmarks run.'
}
ec3['next_entry'] = ec3['next'] = 'C2 RecentProjects then WorkspaceActions/recovery; C1 V05/final plugin qualification remain in final matrix'
p.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n')
