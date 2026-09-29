# Scene projection

`scene_projection` is a static Editor library with no UI/Context dependency. `ScenePresentationHub` shares one
projection for a session/history/configuration/environment key. The author Session is read only during capture;
no live read view escapes. Changed cursors currently use a full immutable capture; a lost cursor rebuilds too.
The cache is bounded, including records waiting for real Runtime retirement. It never drives Runtime frames.

`ViewportPresentation` owns only its view request, output reference, overlay candidate and retirement receipt.
`HighlightRenderer` prepares only after resource capture, keeps pins under backpressure, advances accepted only
on submission, and addresses the actual backend by the complete ViewHandle. RenderResources and the backend
retain their existing GPU completion ownership. Resources and overlay are not author edits.

All calls are on the Runtime owner thread. Environment registrations, fixed asset reads and metadata/code owners
must describe one version. Renderer/resources/executor outlive all consumers and their Runtime retirement.
The original product host remains the only frame driver. Shared projections rebuild together; callers resolve
new instance/camera bindings when ProjectionVersion changes. No persistent raw Entity is an author identity.
