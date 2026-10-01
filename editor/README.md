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

## Existing product, pending P11/P12

The executable has not switched to the new product workflow. `app`, `context`, `ui`, legacy concrete
`tools`, `launcher`, `metadata`, `plugins`, `transition`, and the old editing/save adapters retain only
their registered consumers. Contribution protocol replacement belongs to P11; product and adapter
removal belongs to P12. Formal providers must never include or link those implementations.

The old ProjectCreationPane remains in launcher; its process launch implementation is shared from
application/launch. The integration harness assembles formal modules and is never installed as a second
product. Layout values do not open content; WorkspaceStore returns plans/manifests without applying Root.

See [continuous quality rules](../docs/editor-quality.md). Mutable construction state is only in
`.internal/editor-redesign/`; frozen dev_log snapshots describe their own implementation SHA.
