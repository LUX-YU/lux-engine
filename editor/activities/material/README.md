# Material activities

Existing domain activities, relocated with their separate real targets and ownership.
Author state remains in authoring; workbench and application are consumers, never dependencies.


## Material compilation and preview

`material_preview` owns concrete derived work, not a MaterialSession. Compilation takes an owning MaterialSnapshot,
settings and environment version. A preview destination is not a compile input. CPU work produces one encoded immutable artifact. Completion
is a leaf fact stored unconditionally when ExecutionRuntime dispatches accepted task events, independent of UI
admission. Owners call collectCompletions then dispatchTaskEvents; collection alone does not deliver callbacks.

MaterialPreview accepts an owning completion and a PreviewAdoptionKey (target UUID, adoption generation, input,
recipe generation and environment). MaterialPreviewRecipe is a public owning value: mesh AssetId and immutable
encoded MeshAsset bytes. makeSphereMaterialPreviewRecipe creates the default recipe; an existing encoded mesh
uses the same setDesired(input, recipe) path. Omitting the recipe retains the current selection (first use prepares
the default sphere). Camera, lighting and Feature assembly remain the existing fixed scene setup, not another
renderer or generic recipe language.

An explicit recipe compares mesh identity and exact immutable bytes. Changed bytes with the same AssetId advance
both recipe and adoption generations. Repeated equal inputs are idempotent. Invalid empty inputs leave the current
expectation untouched. New compilation does not reset the chosen mesh. receive binds its owning compiled result
to the recipe fixed by that adoption key; callers may submit the same compiled result for another recipe without
compiling again. Old keys only settle and cannot be relabelled current. MaterialView continues using the default
recipe; this change does not add mesh selection UI.

Prepared, resource-accepted and recorded/sampled output are different facts. Prepared/pending/displayed records
retain their own recipe, not a reference to a mutable desired value. Resource failure or supersession restores both
the last accepted input and its mesh/material descriptor. Existing RenderAssets retains the resolved resource pin
until a replacement succeeds. MaterialPreview owns one existing Runtime lease; ViewportPresentation owns the view.
Neither creates another Runtime or drives frames. Calls are owner-thread only; close preserves the original
retirement protocol.

publishCompiledMaterial reserves the same WriteCoordinator used by source saves and submits the already
encoded artifact. Expected destination version and FIFO remain authoritative. There is no Session pointer or
checkpoint adoption. A source change does not silently relabel an old compiled artifact.
