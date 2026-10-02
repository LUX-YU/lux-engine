# Layout authoring and workspace contracts

Pure values and plans are provided here by layout_model. The file/publication implementation described below is owned by [workspace activities](../../activities/workspace/README.md).


## Workspace values and storage (P09)

`layout_model` contains persistent DockLayout, RecoveryManifest, preferences and immutable validated plans.
It has no live UI, author model, renderer, content loader or file implementation dependency. ViewInfo is the
existing P08 identity value, now published by `editor_contracts` at the same public include path. Recovery
headers belong to this same layout library rather than adding a tiny library.

`workspace_store` borrows the application's P05 WriteCoordinator and IArtifactStore. Production uses the
existing FileArtifactStore. It does not construct a coordinator, drain someone else's ready work or own
an extra queue. `saveLayout`, `renameLayout`, `removeLayout`, `writePreferences` and `writeRecovery` return
accepted WriteTickets. The publication owner takes ready queries, publishes through its backend, calls
complete (including Unknown), reconciles and acknowledges using the original P05 contract. Accepted results
must be delivered independently of new-business admission. A ticket is not a successful disk commit.

`layoutResult` reports the unchanged publication outcome and a separate catalog refresh result.
`preferenceResult` is independent. No failure of preferences or listing erases an earlier committed fact.
The store never changes author checkpoints or uses SaveService to pretend layouts are author sources.

### Persistent format

- `.lux/workspace/layouts/<32 lowercase hex LayoutId>.layout`: schema 1 TOML; label is editable data.
- `.lux/workspace/preferences.toml`: optional selected LayoutId and versioned opaque extensions.
- `.lux/workspace/recovery.toml`: persistent view key/type + content locator; no live ID or source contents.
- `.lux/workspace/migration-v1.toml`: verified legacy source digest, written after all migrated records.

View payloads and opaque extensions carry a schema, exact byte length and hex bytes. Unsupported whole-file
schemas are rejected without writing. Unrecognized fields in supported schemas retain the original file
bytes in an explicit opaque envelope. Limits apply to counts, tree depth, text, payload and encoded file sizes.

Dock nodes are explicit binary split or leaf values with stable local numeric identities. Roots carry geometry
and floating status. Every slot and node must be reachable exactly once. Validation precedes resolve. Plans
own their values, match exact ViewRestoreKey + ViewType, preserve unmatched live views and describe unbound
creation. Provider entries are data, never callbacks. P10/P12 must revalidate transient ViewIds when adopting.

Read failures propagate: BUSY, permission errors, changing files and incomplete enumeration never mean missing.
Missing/corrupt selected layouts produce a diagnostic read-only default choice. Individual bad catalog records
produce an explicitly incomplete catalog; directory failures return an error. No read path writes defaults.

### Publication and removal

All physical targets use the existing root-contained canonical path key; different logical addresses cannot
bypass a lane. Hard links remain unsupported. WriteCoordinator has one additional payload-free REMOVE action.
Removal waits for predecessor retirement, including Unknown; a confirmed predecessor edge may update its
expected version. External versions still conflict. Only FileArtifactStore physically removes the file at
the publication boundary. Removal reports the `missing` version and unconfirmed directory durability, never
claims a flushed file for an unlink. A subsequent explicit new save can recreate a deleted layout; an earlier
pending save cannot run after that lane's deletion.

### Read-only legacy migration

`prepareLegacyMigration` decodes old schema 1 TOML and recognizable ImGui INI geometry without Root or provider
callbacks. Stable layout identities derive from the old filename; per-record provenance records old bytes'
digest. Recognized built-in tool `v1:<asset UUID>` locators enter RecoveryManifest, not view content. Original
TOML (including unknown payloads, auxiliary window state and unsupported INI fields) remains exact opaque data.
Each old file is an independent snapshot: all valid files migrate, but only the explicitly selected file supplies
RecoveryManifest entries. Same PaneId/type in different snapshots can name different assets. Nonselected
locators remain in their original envelopes, not active view state. Empty selection, absent settings or a missing
selected file yields an empty recovery with a diagnostic; access errors still fail without guessing a selection.
The settings digest remains after the sorted layout digests, preserving existing source identities and markers.

`continueMigration` checks real disk and accepts at most one missing record. Its caller must settle that ticket
before the next call. A reconstructed Store can resume after interruption. Existing records with matching
provenance are preserved, including user edits since first import. Other records conflict. Only after every
new record has been reread/validated is the marker accepted. Original `.lux/editor` files are never modified.
Locators do not recover unsaved source contents; this limitation is returned as a diagnostic.
An existing same-origin recovery or completed marker remains authoritative, including user changes. R1 does not
silently replace a previously migrated recovery that used the earlier directory-wide selection policy. Any
later repair of such user data needs a separate explicit decision; no format version changes in this correction.

EditorApplication applies the complete plan through DesktopShell/ViewHost preparation and commit.
Independent RecoveryManifest input supplies content bindings. The product's C01 regression checks
rejected dock data against its original visibility, count and serialized dock state; historical C01
FAIL snapshots remain unchanged and are evaluated at their own implementation SHA.
