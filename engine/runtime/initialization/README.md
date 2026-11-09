# Engine initialization

`engine::init` returns the single owning EngineContext. It creates Execution, Object, a live empty AssetVfs and its repository.
The render-enabled build additionally owns Server, RenderClient, its thread, Main response CompletionWork and RenderFrameSubmission.
Before publishing Context, init connects the notification, starts Server, then creates RenderClient through one
QueryCommandIds batch. This cold wait consumes only its own bootstrap reply and does not pump Main callbacks.
Failure records the stop intent before repository settlement can dispatch Main, cancels queued notification work and joins the service; no partially initialized Client is published.
`Engine` borrows this context and installs frame admission/submission at the start of its one run.

Destroy all scene and external consumers before Context. Registered asset providers must survive repository settlement.
Context destruction returns frame borrows, settles asset uses, joins Render, destroys Object, then releases Execution.
No dispatcher may destroy Context from inside its own callback. Runtime errors during final destruction cannot authorize
unwinding still-borrowed objects. Normal recoverable scene errors retain the Scene in Engine's retirement flow.

`system_render` is static at the Engine integration layer; the modules/function/render library is unchanged.
A build with LUX_BUILD_RENDER=OFF and LUX_ENGINE_WITH_RENDER=OFF has no rendering initialization dependencies.
