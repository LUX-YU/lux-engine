# Shared editor capabilities after EC1

The five responsibility layers remain unchanged. A tool uses the providers for its actual work; it does not
derive from a universal editor or look up services in an application context.

| Responsibility | Public owner |
|---|---|
| Immutable source directory, indexed by AssetId | ProjectCatalogModel / ProjectCatalogSnapshot |
| Fixed VFS source read | ProjectStorage / AssetReadPort |
| Author identity, admission and save checkpoint | SessionStore / domain Session / SessionState |
| History log and identity | EditHistory |
| Execute, undo, redo, clearing and closing a log | EditExecutor, using that same log's gate |
| Read/decode/prepare/install author content | SessionFactorySnapshot / SessionOpening |
| Save admission, ordered publication and adoption | SaveService / WriteCoordinator / domain save role |
| Background execution and reliable completion | ExecutionRuntime / TaskScope |
| UI ownership and mounting | DetachedView / ViewHost; Pane members own their subtrees |
| Typed command lookup and dispatch | CommandRegistry |
| Runtime drive, presentation and retirement | Original SceneRuntime and presentation owners |

EC1 introduced the capability split in V8, EC3 added settings in V9, and EC4 upgrades the current SDK to V10.
The current ABI separates immutable cold contributions from activation. The export declares needed
SessionActivities, ProjectActivities and WorkbenchAccess; missing required capabilities reject activation.
Unrequested groups are not supplied. Activation receives callback-lifetime group pointers and captures only
explicit provider references in its owned closures. Providers outlive admitted work and mounted views.
CodeLease encloses callbacks, dynamic results and their destruction, including rejection and move assignment.

Source selection uses the stable source AssetTypeId name and source format version. Each Session factory may
declare SourceAuthoring, including its save suffix. A View factory declares the session kinds it accepts.
Duplicate defaults are ambiguous; order does not select an editor. A ViewContent contains zero or more SessionIds
and an optional primary. A missing view does not roll back a legally published session. Exact ViewType is kept
during recovery. Historical Manifest and recovery formats retain their read-only migrations.

Built-in Scene, Material and Flow factories use these providers too. They return complete detached views with
their own intent connections. A content view owns local interaction/presentation; it does not own the author
session or checkpoint. Accepted compilation completion and GPU retirement remain with their activity owners
when a view disappears. Derived artifacts use the shared publication role and do not alter author checkpoints.

Scene author applicability comes from declared schemas, installed providers and the actual partition contract.
Inspector, creation tools and render-feature options consult those facts. Installed-but-undeclared is distinct
from unknown persistent data; unknown data is retained. Read failure/BUSY is not an empty set of capabilities.

The external example is `cmake/installed-consumers/editor-ec1-skeleton`. It uses the existing SkeletonAsset
codec and a real plugin-owned Skeleton Session. HEADLESS, WINDOW and APP modes qualify the same DLL through
the installed SDK. No Skeleton branch exists in the host. Its README identifies the qualification-only observer
and the limits of the example; it is not a new runtime skeleton implementation.

Performance changes preserve the same authorities: catalog lookups reuse the immutable index, Outliner
collapse state uses full stable identity, empty component pools are skipped during capture, and admitted opaque
snapshot blocks may be shared while their full logical size remains charged. External inputs still freeze by
copy. Unchanged projections do no rebuild; changed projection updates retain the measured whole-source path.
EC1 does not add a parallel incremental projection framework.

Implementation evidence and exceptions belong only to `.internal/editor-redesign/` and frozen `dev_log/EC1/`.
This document describes the public responsibilities, not an independent status ledger. Prior user waivers,
Linux/IME NOT_RUN and historical performance limitations are not overridden by this design.
