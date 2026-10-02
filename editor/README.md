# Editor

The formal implementation is organized by responsibility, from inner to outer:

| Layer | Source responsibilities | Dependencies |
| --- | --- | --- |
| editing | identities, History, SessionStore, SessionState, shared values | pure foundation |
| authoring | Scene, Material, Flow, Project, Layout values and author models | editing and pure engine/modules |
| activities | save/publication, Run, projection, compilation/preview, project/workspace IO, tasks | formal inner contracts and engine capabilities |
| workbench | desktop, viewport, widgets, domain interaction and views | inner public contracts; no activity private state |
| application | existing process launch and concrete assembly boundaries | explicit providers |

Layers are directories, not five libraries. Targets retain actual CPU, Process, Toolchain and GPU
boundaries. Material/Flow/Scene interactions remain CPU libraries despite residing in workbench.
ProjectBuilder creates a ProjectBuildConfig; only project activities perform file IO.

History has one shared algorithm. SessionStore owns author sessions; each model owns its source and
SessionState. SaveService and WriteCoordinator keep accepted work and publication facts. SceneRuntime
owns runtime instances and retirement. RunStore owns run records; ViewHost alone owns top-level Panes.
Views retain explicit bindings, gestures and local presentation. Closing a View does not close a session,
cancel another owner's task or release its unfinished GPU responsibility.

Public includes/packages retain their logical names. Project-only support lives in sinclude; private
implementation headers are not SDK APIs. Inspector generation lives in workbench/scene; generic widgets
and viewport do not depend on author models. Tests spanning layers are configured last under tests.

## Product assembly

`lux_editor` is the sole installed Editor executable. It constructs EditorApplication, loads the project's
V7 contributions, and combines the existing content, save, compilation, Run and desktop providers.
With no project it displays the same Launcher composition used by `lux_launcher`; project creation uses
ProjectCreationView and the existing asynchronous project IO. No old/new product fallback is built.
The nine old roots and their registration/save bridges have been removed. The formal scene execution
activity retains runtime Registry editing; it is not a legacy author adapter.

Layout application is an atomic Host preparation/commit. Independent recovery manifests supply content
bindings; layout payloads never authorize opening every persisted asset. Read-only legacy data import
remains in WorkspaceStore. Historical qualification snapshots keep their original source SHA and results.

See [continuous quality rules](../docs/editor-quality.md). Mutable construction state is only in
`.internal/editor-redesign/`; frozen dev_log snapshots describe their own implementation SHA.
