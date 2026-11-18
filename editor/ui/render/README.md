# Editor UI rendering extension

This standalone target builds against the installed `lux-engine-render` SDK, the pinned Lux ImGui fork, and the
installed `lux_shader_compile` executable. It has no Render private-header dependency. Link `lux::editor::ui_render`
and call `lux::editor::ui::registerUi(server)` during setup, before starting the server. Registration owns no UI
context. The statically linked extension stays alive through all registered callbacks and resources.

Query the UI implementation from `ListCapabilities`, then call `makeUiRenderEffect` to create its ordinary Feature
recipe. Its View-domain `UiResource` is created with the Effect dependencies. Resolve that Resource via `QueryResource`
with a `ViewRequest`, then send `SubmitDrawData` to its identity. Request spans borrow the producer's ImGui draw arrays
only until the response. A successful command owns the complete converted snapshot; later executions of the UI Job
can render it without the original arrays. Empty draw data replaces the snapshot with no geometry. A failed command
preserves the previous snapshot. Capture `ImDrawData` list spans before the next `NewFrame`; no ImGui context pointer
or arbitrary draw callback is retained. ResetRenderState is accepted, other callbacks are rejected.

`DrawCapture::capture` checks that ImGui has produced valid draw data and reuses its list metadata. Keep that metadata,
the draw arrays and the image token list alive until the `SubmitDrawData` response. Only the pinned fork's 16-bit
index format is accepted by the UI recipe and configuration codec.

Texture/font creation and updates use the existing Texture Resource commands. Image tokens map to a TextureId or a
sampled offscreen TargetId. The Render core resolves these identities, orders current producers before readers and
tracks native reads. Keep a referenced texture alive while its snapshot will be rendered. Stop a producer to sample
its last accepted output; remove all snapshot references before
removing the target. Resizing replaces the physical output, so a producer must publish again before stopped-output
sampling. General Engine/Editor viewport and product integration remain separate.

The four header layers retain the repository conventions; only `include/` is installed. GPU buffer allocation,
staging, descriptors and submission are provided by the core's generic indexed stream path, not a second UI streamer.

To verify the installed package, configure `cmake/installed-consumers/render-extension` with
`-DLUX_TEST_INSTALLED_EDITOR_UI=ON` and point `CMAKE_PREFIX_PATH` at the SDK and ImGui install. Run the
`installed.editor.ui_*` tests for the Offscreen draw, sampled producer output and empty snapshot pixel checks.
