# Declarative application capabilities

This document describes public usage and responsibilities, not qualification status. Evidence and
unexecuted scopes remain in the implementation-specific `dev_log/EC3` record.

## Declarations, handles and code lifetime

One `CommandDescriptor` represents either a fixed declaration or a dynamically frozen declaration.
`CommandEntry::bind<descriptor>(code, query, execute)` requires static constant storage. For runtime text,
`CommandEntry::create` freezes all text in one entry-owned backing. Entry destruction releases callable
objects before its code lease; receiving snapshots keep their own code pin outside plugin destructors.
Session and View factories use the same fixed/dynamic distinction and their existing descriptor types.

`CommandRegistrySnapshot::create` validates identities, default shortcuts and collisions before publication.
`find` is a cold external-name boundary. `at` obtains a handle by an index in that immutable snapshot.
PINNED dispatch uses its retained entry; CURRENT resolves the numeric locator and checks names and input
contract when the defining entry changes. Hash equality never proves cross-version identity by itself.
Original thread, batch, scope, content-source and capacity checks still apply.

Runnable examples (each has its own CMake entry):

- [Fixed and dynamic declarations, pinned handles](../cmake/installed-consumers/editor-ec3-declarations/main.cpp).
- [Project content saving without Application or Root](../cmake/installed-consumers/editor-ec3-project-save/main.cpp).
- [Independent workspace policy](../cmake/installed-consumers/editor-ec3-workspace/main.cpp).
- [External extension capabilities and settings](../cmake/installed-consumers/editor-ec1-skeleton/CMakeLists.txt).

Current Editor extension exports are V9. Runtime/script versions did not change. Fixed metadata is
declared by the implementing module, not registered by static initialization. Contributions are prepared
and adopted under the original participating owners' synchronous guards.

## Settings and window environment

`SettingsDocument` retains schema versions and unknown plugin values. `SettingsEntry` owns the immutable
descriptor/codec binding; `SettingsDraft` couples edited values to their origin and tracks application
separately from persistence. `SettingsPage` keeps the defining entry and page factory code outside the
control. Busy, conflict and publication Unknown preserve the draft and original publication responsibility.

Scopes are installation, project, user, user-project and launch; each descriptor permits a subset.
Project plugin selection still belongs to the project manifest. WorkspaceStore settings publication
uses the existing coordinator; it does not change author checkpoints. The existing settings page has
Apply, Save, Revert and Defaults. Appearance and window placement take effect on restart; shortcuts use
the original menu parser and apply at a safe point. A successful apply is not a successful save.

Native window policy resolves content rectangles against work areas and frame insets using signed
coordinates. Logical UI scale differs from framebuffer scale. Normal placement survives maximized and
fullscreen modes; minimized windows do not overwrite it. Observed changes coalesce behind one admitted
write. Explicit launch overrides are not immediately persisted merely because a window was created.
See [the actual startup/settings consumers](../editor/tests/integration/application/settings.cpp) and
[window implementation](../modules/platform/window/src/LuxWindow.glfw.cpp).

## Camera and generation

ViewportPresentation supports a borrowed existing camera or a view-local camera. Borrowed cameras are
not navigated or destroyed by the viewport. Local navigation patches the original Transform/Camera
components and output request only when values change. Picking uses the sampled output's extent and
requires its camera/request/output identity to match; pending output is deferred rather than combined
with a new camera. Original RenderSystem extraction, receipts and resource retirement remain authoritative.

Inspector generation parses through lux-cxx's original MetaUnit, prepares a structural projection and
renders inja templates for author and Run bindings. Constant C arrays (including aliases) have explicit
IR extents. Numeric annotations retain exact integer bounds. All rendering and formatting completes in
staging before publication. A failed file update cannot create a successful stamp; publication is not
claimed to be an atomic multi-file filesystem transaction.

The [installed generation consumer](../cmake/installed-consumers/editor-inspector-generation/CMakeLists.txt)
uses public headers, installed support and the host executable. Its `qualify.py` drives actual incremental
builds, dependency changes, missing outputs and failing template/formatter/publication paths. The Python
driver does not generate production C++.
