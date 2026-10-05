# Desktop

## EC4 composition

`editor_composition` provides `EditorContext` and the immutable `UiRegistry` factory catalog. It depends
on the neutral services, UI and command providers, with no concrete tool or old Host dependency.
Its lexical root service scope is also used for declared command dependencies. It does not preconstruct
any domain service. Explicit child scopes isolate content/project lifetimes and are passed to UI factories.
The existing `CommandMenu` belongs to this same provider. Menu requests, programmatic commands and
configuration batches can consume the same fixed `UiHandle`; BUSY retains queued input and replacing
the catalog rejects a stale handle instead of silently resolving its name again. The installed
composition consumer exercises this route without importing desktop_shell or view_host.
Factories return complete detached unique owners. `Root::addSubPane`/`addSubPanes` register them and can
transfer ownership into the existing LuxObject parent relation; Root has no second owning window table.
External members and externally owned children remain external and are unlinked when their parent ends.

Content selection uses a declared set of author kinds and an optional preferred window type. Resolution
returns a fixed `UiHandle`, rejects ambiguity and missing preferences, and never constructs a service or
window. Dynamic declarations freeze their kind names in the same backing as their other metadata.
Creation verifies that the selected entry still belongs to the accepted catalog. Configuration batches
retain their captured input, service definitions and Root version through preparation and cleanup.

`UiRegistry::describe` returns owning values with Root's original `PaneHandle`, the creating declaration's
persistent key/type and the explicit content association. Its weak output metadata is not another window
owner. Registry replacement does not substitute the operations of an already created object.
`applyLayout` uses the sole pure `LayoutPlanner`: exact matches prepare local state, new slots use the same
registered factory with empty content, and extra windows survive. It never interprets payloads as asset
locators. State, candidate ownership, visibility and DockTree use the existing Root batch commit before
notifications. A preparation failure preserves the original window set, configuration and author binding.
`captureLayout` fixes the original window handles and Root revision, invokes each creating factory's
state capture under the existing Object borrow, and converts docking through the sole private layout mapping. The returned value uses the existing workspace codec. Callback errors and
structural changes reject the whole capture; it neither reads author source nor creates a recovery
manifest. Catalog replacement does not replace a live window's codec. The old Host layout/capture implementations and ViewFactory package are deleted.

`SessionStore` keeps the sole logical identity/close authority while its actual allocations are shared.
Holding a shared allocation does not restore a closed SessionId. Services and views keep their original
completion, source-stamp, publication and retirement duties.

## Remaining M6 lifecycle regression migration

No production tool or lux_editor link closure imports view_host. The remaining view_host target provides
only the old lifecycle protocol and its dedicated regression consumer until those assertions are migrated.
It owns its IViewHost/ViewError headers directly; the ViewFactory definitions and view_api target/package
are removed. No replacement alias or forwarding package is provided.

The remaining tests cover prepared batch ownership, callback deferral, content associations, permanent
close refusal and explicit retry. These tests are not an alternative product path. Their deletion requires
matching behavior on the actual replacement owners. The original layout preflight, failed second factory,
unchanged visibility/docking, extra-window retention and capture assertions now run in composition.cpp
through UiRegistry and Root, including the installed composition consumer.

desktop_shell supplies platform input, Root, DrawData and the existing UI rendering path. It does not own
author content or drive a second SceneRuntime. DrawData resource uses are fixed before view maintenance
can replace outputs; maintenance and retirement continue without a writable UI frame. Accepted work is
settled by its original service even after a view disappears.

### Related formal modules

| Module | Owns | Borrows |
|---|---|---|
| scene | SceneView camera, Inspector display/interaction buffers, viewport request | SceneSession access, Run inspection, interaction group, shared projection |
| material | Graph/slot display state and preview viewport | MaterialSession access, interaction, MaterialCompilationService and preview owner |
| flow | Graph/property display, last compilation ID | FlowSession access, interaction and compilation service |
| tasks | Task list display | the shared activity TaskMonitor |
| project | Catalog display and pending asset request | ProjectCatalogModel and an explicit AssetReference intent receiver |
| widgets | Virtualized tree/node canvas algorithms | UI callbacks only |

`SessionStore` remains the sole author Session identity/close authority. `RunStore`, SaveService and compilation services retain their original completion and acknowledgement duties. Views own no author source, History, checkpoint or SaveOperation. Layout application organizes existing views; it does not inspect opaque layouts to open content or rewrite recovery/marker files.

The integration executable assembles these formal modules for qualification; only EditorApplication is
installed as the product. No old Context or tool Editor is linked. Inspector generation has one
implementation, producing author and runtime controls against their distinct field capabilities.

P10 native input tests use actual OS mouse/keyboard events through the shell. They do not certify system IME candidate composition; that scope must be recorded separately. The current Editor extension ABI is V10; V9 modules are rejected. Historical input results do not qualify the EC4 product.

## UI lifecycle qualification

Window identity is Root's original PaneHandle; persistent restore keys are separate values. Factory
construction stays detached. Parent-owned windows transfer their actual owner and code pin into the
LuxObject parent relation; external children retain their external owner. Callback borrows defer physical
reclamation to the original Object dispatcher.

`test/lifecycle.cpp` 同时用于安装消费者，执行真实 ImGui Root 的绘制、注册、焦点撤销、离树析构与通知故障。
这不是 P10/P13 GPU 像素、IME 或新产品资格。

`CommandMenu::setShortcuts` prepares a bounded set of user overrides at a safe point. Each row carries the
stable canonical command ID, expected scope and input version; absent extensions keep their inactive rows.
Conflicting effective bindings reject the complete candidate. Fixed command declarations are never edited.
Open menus return BUSY and retain their original handles and text. A rejected registry refresh preserves
the last accepted menu and reports the incompatibility; it does not reparse the same failed version each frame.
