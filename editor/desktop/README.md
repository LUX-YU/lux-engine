# Desktop and view ownership

`view_host` owns complete `DetachedView` units. `Root` registers their UI trees but does not own them. A factory must not register a window or adopt a Session. `desktop_shell` supplies the shared input, Root, DrawData and UI rendering path; it neither drives the SceneRuntime nor owns editing, saving or compilation services.

The application collects accepted background completions, updates the shell, drives its one SceneRuntime, and submits the existing renderer work. The shell fixes DrawData resource uses before view maintenance can replace outputs. With no writable UI frame, maintenance and retirement still proceed.

Host adoption prepares capacity and routes before committing. The host takes the candidate only after preparation succeeds. Notifications run after the structural commit; requests made in callbacks enter the next bounded batch. `ViewAdoption` and `ViewDrain` preserve notification delivery results. Native close intent is coalesced in its existing slot, so a full external queue cannot discard it.

Each owned unit keeps its defining code alive until its Pane and all member/unique-owned children have been destroyed. Closing a view first finishes its `prepareClose` contract, then detaches its routes and focus, then destroys the unit outside callbacks. Temporary BUSY keeps the unit intact. Destruction requires an outer safe point and dependencies that outlive the host; it does not synchronously wait for GPU or executor work. Accepted work belongs to its service, not the disappeared view.

## Related formal modules

| Module | Owns | Borrows |
|---|---|---|
| scene/ui | SceneView camera, Inspector display/interaction buffers, viewport request | SceneSession access, Run inspection, interaction group, shared projection |
| material/ui | Graph/slot display state and preview viewport | MaterialSession access, interaction, application compile requests and preview owner |
| flowforge/ui | Graph/property display, last compilation ID | FlowSession access, interaction and compilation service |
| tasks/ui | Task list display | ExecutionRuntime query/cancel port |
| project/ui | Catalog display and pending asset request | Catalog query and application open port |
| widgets | Virtualized tree/node canvas algorithms | UI callbacks only |

`SessionStore` remains the sole author Session owner. `RunStore`, SaveService and compilation services retain their original completion and acknowledgement duties. Views own no author source, History, checkpoint or SaveOperation. Layout application organizes existing views; it does not inspect opaque layouts to open content or rewrite recovery/marker files.

The P10 integration executable is a noninstalled test assembly of these modules, not another product. The old product uses bounded conversion adapters until P12; new targets do not link `editor_ui`, old Context or tool Editors. The Inspector generator has one implementation in scene/ui, with a temporary old interaction policy for those registered consumers.

P10 native input tests use actual OS mouse/keyboard events through the new shell. They do not certify system IME candidate composition; that scope must be recorded separately. Full product entry switching and dynamic registration remain P12 and P11 respectively.
