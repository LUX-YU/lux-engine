# Application

`editor_bootstrap` composes the formal providers for the installed `lux_editor`. It owns application
lifetimes, explicit provider wiring, content presentation and close review. Actual activities own project
source saving/registration, workspace changes, recovery opening, recent projects and plugin selection.
SessionStore owns author content; SaveService/WriteCoordinator own accepted saves/publications;
RunStore/SceneRuntime own execution. ViewHost alone owns top-level Panes.

`EditorApplication::create` publishes only a fully assembled desktop, including its command receiver.
`execute` is an owner-thread API: another thread receives `WRONG_THREAD` before command lookup or
callbacks; owner-thread reentry receives `BUSY`. Neither rejection admits work or changes content.
The frame receives accepted completions, applies owner maintenance and drives the one SceneRuntime once.
Close decisions retain content stamps. All content and views are prepared before irreversible close;
physical file facts are preserved if another decision cancels. Completion and retirement continue after
windows disappear. No view owns a worker's result or a save operation.

`launchEditor` remains the one process-launch implementation in `editor_launch`. The Launcher uses
formal ProjectCreationView and ProjectCreation; a completed project launches the same installed Editor.
Project creation and native file picking use existing Process schedulers, not UI-owned threads.

V9 contributions provide immutable commands, three content factories and detached view factories.
Standalone extension tools bind `std::monostate` and appear in Window through their exact ViewTypeId.
The runtime plugin loader remains in engine/project; this layer supplies Editor contribution assembly.

Fixed command, content and view metadata belong to their defining modules. The application combines
contributions; it does not interpret tool roles or replace a base Save command. Dynamic window commands
retain their factory snapshot and bounded text storage. CommandMenu borrows that snapshot's immutable
labels and resolves default/overridden shortcuts only at catalog or settings changes.

Settings documents and resolution are pure authoring values. WorkspaceStore publishes settings through
its original WriteCoordinator, independently of layout/recovery/catalog facts. SettingsContent retains
an exact descriptor, draft origin and applied/persisted states. Startup reads explicit sources before
creating a window; absent and unreadable are different outcomes. Personal project state lives under
the user-project profile. Read-only migration validates the complete legacy source set before publishing
a final marker, preserves unknown bytes, and does not rewrite project-side originals.
