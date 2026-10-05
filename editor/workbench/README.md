# Workbench

Desktop, viewport and widgets are generic UI capabilities. Scene, Material, Flow, Project and Tasks
compose domain public services without duplicating author or asynchronous owners. Domain interaction
targets remain CPU-only, regardless of their physical workbench location.

InteractionDelivery is a private shared algorithm for the two graph tools, not a widget or installed API.

The Scene module declares its viewport, creation form, Outliner, author/Run Inspectors, resource view,
and configuration form through the same UiRegistry. Auxiliary factories return complete unique Pane owners;
the Root receives them through Object ownership. They do not construct or own a SessionStore/RunStore.

At their creation boundary, inspection factories borrow the root SessionStore and component schemas.
A lexical child ServiceScope may lend `lux.editor.scene.interaction` as the existing
`shared_ptr<SceneInteractionGroup>`; outputs copy that actual owner and never keep the scope/resolver.
A supplied group's author association must match UiCreateInfo.content. Standalone author views can instead
use that content association to create their own local interaction. Run Inspector uses the supplied Run group
and the root `lux.editor.scene.runs` RunStore. Inspector declares the `lux.editor.scene.editors` definition dependency, then owns its
complete component list and code pins. No contributed definitions means only builtin components.
The resolver cannot construct or discover any undeclared service through this metadata read.

Resource view creation optionally borrows the pair `lux.ui.root` and `lux.editor.scene.viewport` in its
lexical scope. Only the original PaneHandle is retained, and removal/identity reuse never redirects it.
The configuration/creation factories declare their actual plugin, schema, system and render metadata
borrows. Scene-local conversion freezes editor definitions from one contribution snapshot; controls never
re-read the mutable catalog. Project creation calls the same conversion and pins its temporary environment.
Application no longer assembles or owns these control inputs/component lists. Close preparation releases pending
encoded input inside the Session gate without rebuilding controls in Root's callback; an abandoned close
restores the form in ordinary maintenance. Physical Pane reclamation uses the Object dispatcher safe point
before the borrowed infrastructure can be destroyed.

All factories and production consumers now use standard unique Pane owners and Root composition.
The old DetachedView/ViewHost protocols and their installation entries are removed.

ProjectCreation, Settings, Results and Workspace also expose real UiDescriptor factories. They borrow the
existing typed query/request providers (and ProjectStorage/PluginManager where needed), validate the complete
input before constructing controls, and return standard unique Pane owners. Project creation is lazy at the
request-provider boundary and construction does not submit IO. Settings user-intent signals remain on the
actual Pane; their business receiver owns its connections. No factory creates a Host or a parallel task owner.
Results' current builtin action protocol and Settings' saved-value policy still require the EC4 M7 migration;
these construction declarations do not certify that later business work as complete.

### EC4 project intent construction

Project, import and recent-project UiDescriptors accept optional, exact borrowed receiver dependencies.
An absent receiver leaves the public signal available for an explicit caller connection; a registered empty
receiver is rejected before construction. Configured connections belong to the concrete Pane and are
installed before mounting, so configuration and layout construction retain the same input behavior.
The receiver must remain callable for the lifetime of its borrowing window. No native dialog, launch or
asset IO is executed during construction. Import browse carries the current Root PaneHandle, including
its generation; a receiver must retain that identity when delivering a later result. `requestBrowse` and
`requestOpen` report intent delivery, not acceptance or completion of an asynchronous business operation.
Application uses these same factories. The legacy settings protocol and remaining domain-specific
product lifecycle policy still require their designated EC4 migration.
