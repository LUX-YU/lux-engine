# Application

`editor_bootstrap` composes the formal providers for the installed `lux_editor`. It owns application
lifetimes, command wiring, content presentation, workspace/recovery application and close review.
SessionStore owns author content; SaveService/WriteCoordinator own accepted saves/publications;
RunStore/SceneRuntime own execution. ViewHost alone owns top-level Panes.

`EditorApplication::create` publishes only a fully assembled desktop, including its command receiver.
The frame receives accepted completions, applies owner maintenance and drives the one SceneRuntime once.
Close decisions retain content stamps. All content and views are prepared before irreversible close;
physical file facts are preserved if another decision cancels. Completion and retirement continue after
windows disappear. No view owns a worker's result or a save operation.

`launchEditor` remains the one process-launch implementation in `editor_launch`. The Launcher uses
formal ProjectCreationView and ProjectCreation; a completed project launches the same installed Editor.
Project creation and native file picking use existing Process schedulers, not UI-owned threads.

V7 contributions provide immutable commands, three content factories and detached view factories.
Standalone extension tools bind `std::monostate` and appear in Window through their exact ViewTypeId.
The runtime plugin loader remains in engine/project; this layer supplies Editor contribution assembly.
