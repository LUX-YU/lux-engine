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
and the root `lux.editor.scene.runs` RunStore. Optional `lux.editor.scene.inspector.components` supplies the
complete component list (including its code pins); absent contributions use the builtin component list.

Resource view creation optionally borrows the pair `lux.ui.root` and `lux.editor.scene.viewport` in its
lexical scope. Only the original PaneHandle is retained, and removal/identity reuse never redirects it.
The configuration form borrows `lux.editor.scene.configuration`. Its close preparation releases pending
encoded input inside the Session gate without rebuilding controls in Root's callback; an abandoned close
restores the form in ordinary maintenance. Physical Pane reclamation uses the Object dispatcher safe point
before the borrowed infrastructure can be destroyed.

These formal factories do not invoke the legacy DetachedView constructors. Their remaining Application and
legacy test consumers are still scheduled for removal at EC4 M6; the product Host cutover is not complete.
