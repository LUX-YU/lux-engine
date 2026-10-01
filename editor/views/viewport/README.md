# Shared viewport

`editor_viewport` is a STATIC implementation/SDK boundary for ViewportElement, ViewportPresentation,
camera navigation and the private HighlightRenderer. It has no author model, Inspector, concrete tool,
or legacy Context dependency. Scene and Material consumers supply explicit scene, render-system and
camera identities. It never advances Simulation. View-local resources follow the existing
RenderResources/receipt retirement contract. Scene query based creation-point placement remains in scene_ui.
